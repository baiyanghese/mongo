/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/get_executor.h"

#include <limits>

#include "mongo/base/parse_number.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/shard_filter.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/index_names.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    // static
    void filterAllowedIndexEntries(const AllowedIndices& allowedIndices,
                                   std::vector<IndexEntry>* indexEntries) {
        invariant(indexEntries);

        // Filter index entries
        // Check BSON objects in AllowedIndices::_indexKeyPatterns against IndexEntry::keyPattern.
        // Removes IndexEntrys that do not match _indexKeyPatterns.
        std::vector<IndexEntry> temp;
        for (std::vector<IndexEntry>::const_iterator i = indexEntries->begin();
             i != indexEntries->end(); ++i) {
            const IndexEntry& indexEntry = *i;
            for (std::vector<BSONObj>::const_iterator j = allowedIndices.indexKeyPatterns.begin();
                 j != allowedIndices.indexKeyPatterns.end(); ++j) {
                const BSONObj& index = *j;
                // Copy index entry to temp vector if found in query settings.
                if (0 == indexEntry.keyPattern.woCompare(index)) {
                    temp.push_back(indexEntry);
                    break;
                }
            }
        }

        // Update results.
        temp.swap(*indexEntries);
    }

    namespace {
        // The body is below in the "count hack" section but getRunner calls it.
        bool turnIxscanIntoCount(QuerySolution* soln);
    }  // namespace


    void fillOutPlannerParams(Collection* collection,
                              CanonicalQuery* canonicalQuery,
                              QueryPlannerParams* plannerParams) {
        // If it's not NULL, we may have indices.  Access the catalog and fill out IndexEntry(s)
        IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(false);
        while (ii.more()) {
            const IndexDescriptor* desc = ii.next();
            plannerParams->indices.push_back(IndexEntry(desc->keyPattern(),
                                                        desc->getAccessMethodName(),
                                                        desc->isMultikey(),
                                                        desc->isSparse(),
                                                        desc->indexName(),
                                                        desc->infoObj()));
        }

        // If query supports index filters, filter params.indices by indices in query settings.
        QuerySettings* querySettings = collection->infoCache()->getQuerySettings();
        AllowedIndices* allowedIndicesRaw;

        // Filter index catalog if index filters are specified for query.
        // Also, signal to planner that application hint should be ignored.
        if (querySettings->getAllowedIndices(*canonicalQuery, &allowedIndicesRaw)) {
            boost::scoped_ptr<AllowedIndices> allowedIndices(allowedIndicesRaw);
            filterAllowedIndexEntries(*allowedIndices, &plannerParams->indices);
            plannerParams->indexFiltersApplied = true;
        }

        // We will not output collection scans unless there are no indexed solutions. NO_TABLE_SCAN
        // overrides this behavior by not outputting a collscan even if there are no indexed
        // solutions.
        if (storageGlobalParams.noTableScan) {
            const string& ns = canonicalQuery->ns();
            // There are certain cases where we ignore this restriction:
            bool ignore = canonicalQuery->getQueryObj().isEmpty()
                          || (string::npos != ns.find(".system."))
                          || (0 == ns.find("local."));
            if (!ignore) {
                plannerParams->options |= QueryPlannerParams::NO_TABLE_SCAN;
            }
        }

        // If the caller wants a shard filter, make sure we're actually sharded.
        if (plannerParams->options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            CollectionMetadataPtr collMetadata =
                shardingState.getCollectionMetadata(canonicalQuery->ns());

            if (collMetadata) {
                plannerParams->shardKey = collMetadata->getKeyPattern();
            }
            else {
                // If there's no metadata don't bother w/the shard filter since we won't know what
                // the key pattern is anyway...
                plannerParams->options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
        }

        if (internalQueryPlannerEnableIndexIntersection) {
            plannerParams->options |= QueryPlannerParams::INDEX_INTERSECTION;
        }

        plannerParams->options |= QueryPlannerParams::KEEP_MUTATIONS;
        plannerParams->options |= QueryPlannerParams::SPLIT_LIMITED_SORT;
    }

    Status getExecutorIDHack(Collection* collection,
                             CanonicalQuery* query,
                             const QueryPlannerParams& plannerParams,
                             PlanExecutor** out) {
        invariant(collection);

        LOG(2) << "Using idhack: " << query->toStringShort();
        WorkingSet* ws = new WorkingSet();
        PlanStage* root = new IDHackStage(collection, query, ws);

        // Might have to filter out orphaned docs.
        if (plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            root = new ShardFilterStage(shardingState.getCollectionMetadata(collection->ns()),
                                        ws, root);
        }

        // There might be a projection. The idhack stage will always fetch the full document,
        // so we don't support covered projections. However, we might use the simple inclusion
        // fast path.
        if (NULL != query && NULL != query->getProj()) {
            ProjectionStageParams params(WhereCallbackReal(collection->ns().db()));
            params.projObj = query->getProj()->getProjObj();

            // Stuff the right data into the params depending on what proj impl we use.
            if (query->getProj()->requiresDocument() || query->getProj()->wantIndexKey()) {
                params.fullExpression = query->root();
                params.projImpl = ProjectionStageParams::NO_FAST_PATH;
            }
            else {
                params.projImpl = ProjectionStageParams::SIMPLE_DOC;
            }

            root = new ProjectionStage(params, ws, root);
        }

        *out = new PlanExecutor(ws, root, collection);
        return Status::OK();
    }

    Status getExecutor(Collection* collection,
                      CanonicalQuery* canonicalQuery,
                      PlanExecutor** out,
                      size_t plannerOptions) {
        invariant(canonicalQuery);

        // This can happen as we're called by internal clients as well.
        if (NULL == collection) {
            const string& ns = canonicalQuery->ns();
            LOG(2) << "Collection " << ns << " does not exist."
                   << " Using EOF runner: " << canonicalQuery->toStringShort();
            EOFStage* eofStage = new EOFStage();
            WorkingSet* ws = new WorkingSet();
            *out = new PlanExecutor(ws, eofStage, collection);
            return Status::OK();
        }

        // Fill out the planning params.  We use these for both cached solutions and non-cached.
        QueryPlannerParams plannerParams;
        plannerParams.options = plannerOptions;
        fillOutPlannerParams(collection, canonicalQuery, &plannerParams);

        // If we have an _id index we can use the idhack runner.
        if (IDHackStage::supportsQuery(*canonicalQuery) &&
            collection->getIndexCatalog()->findIdIndex()) {
            return getExecutorIDHack(collection, canonicalQuery, plannerParams, out);
        }

        // Tailable: If the query requests tailable the collection must be capped.
        if (canonicalQuery->getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!collection->isCapped()) {
                return Status(ErrorCodes::BadValue,
                              "error processing query: " + canonicalQuery->toString() +
                              " tailable cursor requested on non capped collection");
            }

            // If a sort is specified it must be equal to expectedSort.
            const BSONObj expectedSort = BSON("$natural" << 1);
            const BSONObj& actualSort = canonicalQuery->getParsed().getSort();
            if (!actualSort.isEmpty() && !(actualSort == expectedSort)) {
                return Status(ErrorCodes::BadValue,
                              "error processing query: " + canonicalQuery->toString() +
                              " invalid sort specified for tailable cursor: "
                              + actualSort.toString());
            }
        }

        // Try to look up a cached solution for the query.

        CachedSolution* rawCS;
        if (PlanCache::shouldCacheQuery(*canonicalQuery) &&
            collection->infoCache()->getPlanCache()->get(*canonicalQuery, &rawCS).isOK()) {
            // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
            boost::scoped_ptr<CachedSolution> cs(rawCS);
            QuerySolution *qs, *backupQs;
            QuerySolution*& chosenSolution=qs; // either qs or backupQs
            Status status = QueryPlanner::planFromCache(*canonicalQuery, plannerParams, *cs,
                                                        &qs, &backupQs);

            if (status.isOK()) {
                // the working set will be shared by the root and backupRoot plans
                // and owned by the containing single-solution-runner
                //
                WorkingSet* sharedWs = new WorkingSet();

                PlanStage *root, *backupRoot=NULL;
                verify(StageBuilder::build(collection, *qs, sharedWs, &root));
                if ((plannerParams.options & QueryPlannerParams::PRIVATE_IS_COUNT)
                    && turnIxscanIntoCount(qs)) {
                    LOG(2) << "Using fast count: " << canonicalQuery->toStringShort()
                           << ", planSummary: " << getPlanSummary(*qs);

                    if (NULL != backupQs) {
                        delete backupQs;
                    }
                }
                else if (NULL != backupQs) {
                    verify(StageBuilder::build(collection, *backupQs, sharedWs, &backupRoot));
                }

                // add a CachedPlanStage on top of the previous root
                root = new CachedPlanStage(collection, canonicalQuery, root, backupRoot);

                *out = new PlanExecutor(sharedWs, root, chosenSolution, collection);
                return Status::OK();
            }
        }

        if (internalQueryPlanOrChildrenIndependently
            && SubplanStage::canUseSubplanning(*canonicalQuery)) {

            QLOG() << "Running query as sub-queries: " << canonicalQuery->toStringShort();
            LOG(2) << "Running query as sub-queries: " << canonicalQuery->toStringShort();

            auto_ptr<WorkingSet> ws(new WorkingSet());

            SubplanStage* subplan;
            Status runnerStatus = SubplanStage::make(collection, ws.get(), plannerParams,
                                                     canonicalQuery, &subplan);
            if (!runnerStatus.isOK()) {
                return runnerStatus;
            }

            *out = new PlanExecutor(ws.release(), subplan, collection);
            return Status::OK();
        }

        return getExecutorAlwaysPlan(collection, canonicalQuery, plannerParams, out);
    }

    Status getExecutorAlwaysPlan(Collection* collection,
                                 CanonicalQuery* canonicalQuery,
                                 const QueryPlannerParams& plannerParams,
                                 PlanExecutor** execOut) {
        invariant(collection);
        invariant(canonicalQuery);

        *execOut = NULL;

        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*canonicalQuery, plannerParams, &solutions);
        if (!status.isOK()) {
            return Status(ErrorCodes::BadValue,
                          "error processing query: " + canonicalQuery->toString() +
                          " planner returned error: " + status.reason());
        }

        // We cannot figure out how to answer the query.  Perhaps it requires an index
        // we do not have?
        if (0 == solutions.size()) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                          << "error processing query: "
                          << canonicalQuery->toString()
                          << " No query solutions");
        }

        // See if one of our solutions is a fast count hack in disguise.
        if (plannerParams.options & QueryPlannerParams::PRIVATE_IS_COUNT) {
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (turnIxscanIntoCount(solutions[i])) {
                    // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                    for (size_t j = 0; j < solutions.size(); ++j) {
                        if (j != i) {
                            delete solutions[j];
                        }
                    }

                    LOG(2) << "Using fast count: " << canonicalQuery->toStringShort()
                           << ", planSummary: " << getPlanSummary(*solutions[i]);

                    // We're not going to cache anything that's fast count.
                    WorkingSet* ws = new WorkingSet();
                    PlanStage* root;
                    verify(StageBuilder::build(collection, *solutions[i], ws, &root));

                    *execOut = new PlanExecutor(ws, root, solutions[i], collection);
                    return Status::OK();
                }
            }
        }

        if (1 == solutions.size()) {
            LOG(2) << "Only one plan is available; it will be run but will not be cached. "
                   << canonicalQuery->toStringShort()
                   << ", planSummary: " << getPlanSummary(*solutions[0]);

            // Only one possible plan.  Run it.  Build the stages from the solution.
            WorkingSet* ws = new WorkingSet();
            PlanStage* root;
            verify(StageBuilder::build(collection, *solutions[0], ws, &root));

            *execOut = new PlanExecutor(ws, root, solutions[0], collection);
            return Status::OK();
        }
        else {
            // Many solutions.  Create a MultiPlanStage to pick the best, update the cache, and so on.

            // The working set will be shared by all candidate plans and owned by the containing runner
            WorkingSet* sharedWorkingSet = new WorkingSet();

            MultiPlanStage* multiPlanStage = new MultiPlanStage(collection, canonicalQuery);

            for (size_t ix = 0; ix < solutions.size(); ++ix) {
                if (solutions[ix]->cacheData.get()) {
                    solutions[ix]->cacheData->indexFilterApplied = plannerParams.indexFiltersApplied;
                }

                // version of StageBuild::build when WorkingSet is shared
                PlanStage* nextPlanRoot;
                verify(StageBuilder::build(collection, *solutions[ix],
                                           sharedWorkingSet, &nextPlanRoot));

                // Owns none of the arguments
                multiPlanStage->addPlan(solutions[ix], nextPlanRoot, sharedWorkingSet);
            }

            PlanExecutor* exec = new PlanExecutor(sharedWorkingSet, multiPlanStage, collection);

            *execOut = exec;
            return Status::OK();
        }
    }

    //
    // Count hack
    //

    namespace {

        /**
         * Returns 'true' if the provided solution 'soln' can be rewritten to use
         * a fast counting stage.  Mutates the tree in 'soln->root'.
         *
         * Otherwise, returns 'false'.
         */
        bool turnIxscanIntoCount(QuerySolution* soln) {
            QuerySolutionNode* root = soln->root.get();

            // Root should be a fetch w/o any filters.
            if (STAGE_FETCH != root->getType()) {
                return false;
            }

            if (NULL != root->filter.get()) {
                return false;
            }

            // Child should be an ixscan.
            if (STAGE_IXSCAN != root->children[0]->getType()) {
                return false;
            }

            IndexScanNode* isn = static_cast<IndexScanNode*>(root->children[0]);

            // No filters allowed and side-stepping isSimpleRange for now.  TODO: do we ever see
            // isSimpleRange here?  because we could well use it.  I just don't think we ever do see
            // it.

            if (NULL != isn->filter.get() || isn->bounds.isSimpleRange) {
                return false;
            }

            // Make sure the bounds are OK.
            BSONObj startKey;
            bool startKeyInclusive;
            BSONObj endKey;
            bool endKeyInclusive;

            if (!IndexBoundsBuilder::isSingleInterval( isn->bounds,
                                                       &startKey,
                                                       &startKeyInclusive,
                                                       &endKey,
                                                       &endKeyInclusive )) {
                return false;
            }

            // Make the count node that we replace the fetch + ixscan with.
            CountNode* cn = new CountNode();
            cn->indexKeyPattern = isn->indexKeyPattern;
            cn->startKey = startKey;
            cn->startKeyInclusive = startKeyInclusive;
            cn->endKey = endKey;
            cn->endKeyInclusive = endKeyInclusive;
            // Takes ownership of 'cn' and deletes the old root.
            soln->root.reset(cn);
            return true;
        }

        /**
         * Returns true if indices contains an index that can be
         * used with DistinctNode. Sets indexOut to the array index
         * of PlannerParams::indices.
         * Look for the index for the fewest fields.
         * Criteria for suitable index is that the index cannot be special
         * (geo, hashed, text, ...).
         *
         * Multikey indices are not suitable for DistinctNode when the projection
         * is on an array element. Arrays are flattened in a multikey index which
         * makes it impossible for the distinct scan stage (plan stage generated from
         * DistinctNode) to select the requested element by array index.
         *
         * Multikey indices cannot be used for the fast distinct hack if the field is dotted.
         * Currently the solution generated for the distinct hack includes a projection stage and
         * the projection stage cannot be covered with a dotted field.
         */
        bool getDistinctNodeIndex(const std::vector<IndexEntry>& indices,
                                  const std::string& field, size_t* indexOut) {
            invariant(indexOut);
            bool isDottedField = str::contains(field, '.');
            int minFields = std::numeric_limits<int>::max();
            for (size_t i = 0; i < indices.size(); ++i) {
                // Skip special indices.
                if (!IndexNames::findPluginName(indices[i].keyPattern).empty()) {
                    continue;
                }
                // Skip multikey indices if we are projecting on a dotted field.
                if (indices[i].multikey && isDottedField) {
                    continue;
                }
                int nFields = indices[i].keyPattern.nFields();
                // Pick the index with the lowest number of fields.
                if (nFields < minFields) {
                    minFields = nFields;
                    *indexOut = i;
                }
            }
            return minFields != std::numeric_limits<int>::max();
        }

        /**
         * Checks dotted field for a projection and truncates the
         * field name if we could be projecting on an array element.
         * Sets 'isIDOut' to true if the projection is on a sub document of _id.
         * For example, _id.a.2, _id.b.c.
         */
        std::string getProjectedDottedField(const std::string& field, bool* isIDOut) {
            // Check if field contains an array index.
            std::vector<std::string> res;
            mongo::splitStringDelim(field, &res, '.');

            // Since we could exit early from the loop,
            // we should check _id here and set '*isIDOut' accordingly.
            *isIDOut = ("_id" == res[0]);

            // Skip the first dotted component. If the field starts
            // with a number, the number cannot be an array index.
            int arrayIndex = 0;
            for (size_t i = 1; i < res.size(); ++i) {
                if (mongo::parseNumberFromStringWithBase(res[i], 10, &arrayIndex).isOK()) {
                    // Array indices cannot be negative numbers (this is not $slice).
                    // Negative numbers are allowed as field names.
                    if (arrayIndex >= 0) {
                        // Generate prefix of field up to (but not including) array index.
                        std::vector<std::string> prefixStrings(res);
                        prefixStrings.resize(i);
                        // Reset projectedField. Instead of overwriting, joinStringDelim() appends joined string
                        // to the end of projectedField.
                        std::string projectedField;
                        mongo::joinStringDelim(prefixStrings, &projectedField, '.');
                        return projectedField;
                    }
                }
            }

            return field;
        }

        /**
         * Creates a projection spec for a distinct command from the requested field.
         * In most cases, the projection spec will be {_id: 0, key: 1}.
         * The exceptions are:
         * 1) When the requested field is '_id', the projection spec will {_id: 1}.
         * 2) When the requested field could be an array element (eg. a.0),
         *    the projected field will be the prefix of the field up to the array element.
         *    For example, a.b.2 => {_id: 0, 'a.b': 1}
         *    Note that we can't use a $slice projection because the distinct command filters
         *    the results from the runner using the dotted field name. Using $slice will
         *    re-order the documents in the array in the results.
         */
        BSONObj getDistinctProjection(const std::string& field) {
            std::string projectedField(field);

            bool isID = false;
            if ("_id" == field) {
                isID = true;
            }
            else if (str::contains(field, '.')) {
                projectedField = getProjectedDottedField(field, &isID);
            }
            BSONObjBuilder bob;
            if (!isID) {
                bob.append("_id", 0);
            }
            bob.append(projectedField, 1);
            return bob.obj();
        }

    }  // namespace

    Status getExecutorCount(Collection* collection,
                            const BSONObj& query,
                            const BSONObj& hintObj,
                            PlanExecutor** execOut) {
        invariant(collection);

        const WhereCallbackReal whereCallback(collection->ns().db());

        CanonicalQuery* cq;
        uassertStatusOK(CanonicalQuery::canonicalize(collection->ns().ns(),
                                                     query,
                                                     BSONObj(),
                                                     BSONObj(),
                                                     0,
                                                     0,
                                                     hintObj,
                                                     &cq,
                                                     whereCallback));

        scoped_ptr<CanonicalQuery> cleanupCq(cq);

        return getExecutor(collection, cq, execOut, QueryPlannerParams::PRIVATE_IS_COUNT);
    }

    //
    // Distinct hack
    //

    bool turnIxscanIntoDistinctIxscan(QuerySolution* soln, const string& field) {
        QuerySolutionNode* root = soln->root.get();

        // We're looking for a project on top of an ixscan.
        if (STAGE_PROJECTION == root->getType() && (STAGE_IXSCAN == root->children[0]->getType())) {
            IndexScanNode* isn = static_cast<IndexScanNode*>(root->children[0]);

            // An additional filter must be applied to the data in the key, so we can't just skip
            // all the keys with a given value; we must examine every one to find the one that (may)
            // pass the filter.
            if (NULL != isn->filter.get()) {
                return false;
            }

            // We only set this when we have special query modifiers (.max() or .min()) or other
            // special cases.  Don't want to handle the interactions between those and distinct.
            // Don't think this will ever really be true but if it somehow is, just ignore this
            // soln.
            if (isn->bounds.isSimpleRange) {
                return false;
            }

            // Make a new DistinctNode.  We swap this for the ixscan in the provided solution.
            DistinctNode* dn = new DistinctNode();
            dn->indexKeyPattern = isn->indexKeyPattern;
            dn->direction = isn->direction;
            dn->bounds = isn->bounds;

            // Figure out which field we're skipping to the next value of.  TODO: We currently only
            // try to distinct-hack when there is an index prefixed by the field we're distinct-ing
            // over.  Consider removing this code if we stick with that policy.
            dn->fieldNo = 0;
            BSONObjIterator it(isn->indexKeyPattern);
            while (it.more()) {
                if (field == it.next().fieldName()) {
                    break;
                }
                dn->fieldNo++;
            }

            // Delete the old index scan, set the child of project to the fast distinct scan.
            delete root->children[0];
            root->children[0] = dn;
            return true;
        }

        return false;
    }

    Status getExecutorDistinct(Collection* collection,
                               const BSONObj& query,
                               const std::string& field,
                               PlanExecutor** out) {
        // This should'a been checked by the distinct command.
        invariant(collection);

        // TODO: check for idhack here?

        // When can we do a fast distinct hack?
        // 1. There is a plan with just one leaf and that leaf is an ixscan.
        // 2. The ixscan indexes the field we're interested in.
        // 2a: We are correct if the index contains the field but for now we look for prefix.
        // 3. The query is covered/no fetch.
        //
        // We go through normal planning (with limited parameters) to see if we can produce
        // a soln with the above properties.

        QueryPlannerParams plannerParams;
        plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;

        IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(false);
        while (ii.more()) {
            const IndexDescriptor* desc = ii.next();
            // The distinct hack can work if any field is in the index but it's not always clear
            // if it's a win unless it's the first field.
            if (desc->keyPattern().firstElement().fieldName() == field) {
                plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                           desc->getAccessMethodName(),
                                                           desc->isMultikey(),
                                                           desc->isSparse(),
                                                           desc->indexName(),
                                                           desc->infoObj()));
            }
        }

        const WhereCallbackReal whereCallback(collection->ns().db());

        // If there are no suitable indices for the distinct hack bail out now into regular planning
        // with no projection.
        if (plannerParams.indices.empty()) {
            CanonicalQuery* cq;
            Status status = CanonicalQuery::canonicalize(
                                collection->ns().ns(), query, &cq, whereCallback);
            if (!status.isOK()) {
                return status;
            }

            scoped_ptr<CanonicalQuery> cleanupCq(cq);

            // Does not take ownership of its args.
            return getExecutor(collection, cq, out);
        }

        //
        // If we're here, we have an index prefixed by the field we're distinct-ing over.
        //

        // Applying a projection allows the planner to try to give us covered plans that we can turn
        // into the projection hack.  getDistinctProjection deals with .find() projection semantics
        // (ie _id:1 being implied by default).
        BSONObj projection = getDistinctProjection(field);

        // Apply a projection of the key.  Empty BSONObj() is for the sort.
        CanonicalQuery* cq;
        Status status = CanonicalQuery::canonicalize(collection->ns().ns(),
                                                     query,
                                                     BSONObj(),
                                                     projection,
                                                     &cq,
                                                     whereCallback);
        if (!status.isOK()) {
            return status;
        }

        scoped_ptr<CanonicalQuery> cleanupCq(cq);

        // If there's no query, we can just distinct-scan one of the indices.
        // Not every index in plannerParams.indices may be suitable. Refer to
        // getDistinctNodeIndex().
        size_t distinctNodeIndex = 0;
        if (query.isEmpty() &&
            getDistinctNodeIndex(plannerParams.indices, field, &distinctNodeIndex)) {
            DistinctNode* dn = new DistinctNode();
            dn->indexKeyPattern = plannerParams.indices[distinctNodeIndex].keyPattern;
            dn->direction = 1;
            IndexBoundsBuilder::allValuesBounds(dn->indexKeyPattern, &dn->bounds);
            dn->fieldNo = 0;

            QueryPlannerParams params;

            // Takes ownership of 'dn'.
            QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(*cq, params, dn);
            invariant(soln);

            LOG(2) << "Using fast distinct: " << cq->toStringShort()
                   << ", planSummary: " << getPlanSummary(*soln);

            WorkingSet* ws = new WorkingSet();
            PlanStage* root;
            verify(StageBuilder::build(collection, *soln, ws, &root));
            // Takes ownership of 'ws', 'root', and 'soln'.
            *out = new PlanExecutor(ws, root, soln, collection);
            return Status::OK();
        }

        // See if we can answer the query in a fast-distinct compatible fashion.
        vector<QuerySolution*> solutions;
        status = QueryPlanner::plan(*cq, plannerParams, &solutions);
        if (!status.isOK()) {
            return getExecutor(collection, cq, out);
        }

        // We look for a solution that has an ixscan we can turn into a distinctixscan
        for (size_t i = 0; i < solutions.size(); ++i) {
            if (turnIxscanIntoDistinctIxscan(solutions[i], field)) {
                // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                for (size_t j = 0; j < solutions.size(); ++j) {
                    if (j != i) {
                        delete solutions[j];
                    }
                }

                LOG(2) << "Using fast distinct: " << cq->toStringShort()
                       << ", planSummary: " << getPlanSummary(*solutions[i]);

                // Build and return the SSR over solutions[i].
                WorkingSet* ws = new WorkingSet();
                PlanStage* root;
                verify(StageBuilder::build(collection, *solutions[i], ws, &root));
                // Takes ownership of 'ws', 'root', and 'solutions[i]'.
                *out = new PlanExecutor(ws, root, solutions[i], collection);
                return Status::OK();
            }
        }

        // If we're here, the planner made a soln with the restricted index set but we couldn't
        // translate any of them into a distinct-compatible soln.  So, delete the solutions and just
        // go through normal planning.
        for (size_t i = 0; i < solutions.size(); ++i) {
            delete solutions[i];
        }

        // We drop the projection from the 'cq'.  Unfortunately this is not trivial.
        status = CanonicalQuery::canonicalize(collection->ns().ns(), query, &cq, whereCallback);
        if (!status.isOK()) {
            return status;
        }

        cleanupCq.reset(cq);

        // Does not take ownership.
        return getExecutor(collection, cq, out);
    }

}  // namespace mongo
