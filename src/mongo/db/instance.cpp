// instance.cpp 

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include <boost/filesystem/operations.hpp>
#include <boost/thread/thread.hpp>
#include <fstream>
#if defined(_WIN32)
#include <io.h>
#else
#include <sys/file.h>
#endif

#include "mongo/base/status.h"
#include "mongo/bson/util/atomic_int.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/background.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_recover.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/global_optime.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/commands/count.h"
#include "mongo/db/ops/delete_executor.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_executor.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage_options.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/gcov.h"
#include "mongo/util/goodies.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
    
    // for diaglog
    inline void opread(Message& m) { if( _diaglog.getLevel() & 2 ) _diaglog.readop((char *) m.singleData(), m.header()->len); }
    inline void opwrite(Message& m) { if( _diaglog.getLevel() & 1 ) _diaglog.writeop((char *) m.singleData(), m.header()->len); }

    void receivedKillCursors(OperationContext* txn, Message& m);
    void receivedUpdate(OperationContext* txn, Message& m, CurOp& op);
    void receivedDelete(OperationContext* txn, Message& m, CurOp& op);
    void receivedInsert(OperationContext* txn, Message& m, CurOp& op);
    bool receivedGetMore(OperationContext* txn, DbResponse& dbresponse, Message& m, CurOp& curop );

    int nloggedsome = 0;
#define LOGWITHRATELIMIT if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    string dbExecCommand;

    int lockFile = 0;
#ifdef _WIN32
    HANDLE lockFileHandle;
#endif

    MONGO_FP_DECLARE(rsStopGetMore);

    static void inProgCmd( Message &m, DbResponse &dbresponse ) {
        DbMessage d(m);
        QueryMessage q(d);
        BSONObjBuilder b;

        const bool isAuthorized = cc().getAuthorizationSession()->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::inprog);

        audit::logInProgAuthzCheck(
                &cc(), q.query, isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);

        if (!isAuthorized) {
            b.append("err", "unauthorized");
        }
        else {
            bool all = q.query["$all"].trueValue();
            vector<BSONObj> vals;
            {
                BSONObj filter;
                {
                    BSONObjBuilder b;
                    BSONObjIterator i( q.query );
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        if ( str::equals( "$all", e.fieldName() ) )
                            continue;
                        b.append( e );
                    }
                    filter = b.obj();
                }

                const NamespaceString nss(d.getns());

                Client& me = cc();
                scoped_lock bl(Client::clientsMutex);
                Matcher m(filter, WhereCallbackReal(nss.db()));
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    verify( c );
                    CurOp* co = c->curop();
                    if ( c == &me && !co ) {
                        continue;
                    }
                    verify( co );
                    if( all || co->displayInCurop() ) {
                        BSONObjBuilder infoBuilder;
                        co->reportState(&infoBuilder);

                        const BSONObj info = infoBuilder.obj();
                        if ( all || m.matches( info )) {
                            vals.push_back( info );
                        }
                    }
                }
            }
            b.append("inprog", vals);
            if( lockedForWriting() ) {
                b.append("fsyncLock", true);
                b.append("info", "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
            }
        }

        replyToQuery(0, m, dbresponse, b.obj());
    }

    void killOp( Message &m, DbResponse &dbresponse ) {
        DbMessage d(m);
        QueryMessage q(d);
        BSONObj obj;

        const bool isAuthorized = cc().getAuthorizationSession()->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::killop);
        audit::logKillOpAuthzCheck(&cc(),
                                   q.query,
                                   isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
        if (!isAuthorized) {
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        /*else if( !dbMutexInfo.isLocked() )
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
            */
        else {
            BSONElement e = q.query.getField("op");
            if( !e.isNumber() ) {
                obj = fromjson("{\"err\":\"no op number field specified?\"}");
            }
            else {
                log() << "going to kill op: " << e << endl;
                obj = fromjson("{\"info\":\"attempting to kill op\"}");
                getGlobalEnvironment()->killOperation( (unsigned) e.number() );
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    bool _unlockFsync();
    static void unlockFsync(const char *ns, Message& m, DbResponse &dbresponse) {
        BSONObj obj;

        const bool isAuthorized = cc().getAuthorizationSession()->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::unlock);
        audit::logFsyncUnlockAuthzCheck(
                &cc(), isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
        if (!isAuthorized) {
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else if (strncmp(ns, "admin.", 6) != 0 ) {
            obj = fromjson("{\"err\":\"unauthorized - this command must be run against the admin DB\"}");
        }
        else {
            log() << "command: unlock requested" << endl;
            if( _unlockFsync() ) {
                obj = fromjson("{ok:1,\"info\":\"unlock completed\"}");
            }
            else {
                obj = fromjson("{ok:0,\"errmsg\":\"not locked\"}");
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    static bool receivedQuery(OperationContext* txn, Client& c, DbResponse& dbresponse, Message& m ) {
        bool ok = true;
        MSGID responseTo = m.header()->id;

        DbMessage d(m);
        QueryMessage q(d);
        auto_ptr< Message > resp( new Message() );

        CurOp& op = *(c.curop());

        scoped_ptr<AssertionException> ex;

        try {
            NamespaceString ns(d.getns());
            if (!ns.isCommand()) {
                // Auth checking for Commands happens later.
                Client* client = &cc();
                Status status = client->getAuthorizationSession()->checkAuthForQuery(ns, q.query);
                audit::logQueryAuthzCheck(client, ns, q.query, status.code());
                uassertStatusOK(status);
            }
            dbresponse.exhaustNS = newRunQuery(txn, m, q, op, *resp);
            verify( !resp->empty() );
        }
        catch ( SendStaleConfigException& e ){
            ex.reset( new SendStaleConfigException( e.getns(), e.getInfo().msg, e.getVersionReceived(), e.getVersionWanted() ) );
            ok = false;
        }
        catch ( AssertionException& e ) {
            ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
            ok = false;
        }

        if( ex ){

            op.debug().exceptionInfo = ex->getInfo();
            log() << "assertion " << ex->toString() << " ns:" << q.ns << " query:" <<
                (q.query.valid() ? q.query.toString() : "query object is corrupt") << endl;
            if( q.ntoskip || q.ntoreturn )
                log() << " ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << endl;

            SendStaleConfigException* scex = NULL;
            if ( ex->getCode() == SendStaleConfigCode ) scex = static_cast<SendStaleConfigException*>( ex.get() );

            BSONObjBuilder err;
            ex->getInfo().append( err );
            if( scex ){
                err.append( "ns", scex->getns() );
                scex->getVersionReceived().addToBSON( err, "vReceived" );
                scex->getVersionWanted().addToBSON( err, "vWanted" );
            }
            BSONObj errObj = err.done();

            if( scex ){
                log() << "stale version detected during query over "
                      << q.ns << " : " << errObj << endl;
            }

            BufBuilder b;
            b.skip(sizeof(QueryResult));
            b.appendBuf((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            QueryResult * msgdata = (QueryResult *) b.buf();
            b.decouple();
            QueryResult *qr = msgdata;
            qr->_resultFlags() = ResultFlag_ErrSet;
            if( scex ) qr->_resultFlags() |= ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            resp.reset( new Message() );
            resp->setData( msgdata, true );

        }

        op.debug().responseLength = resp->header()->dataLen();

        dbresponse.response = resp.release();
        dbresponse.responseTo = responseTo;

        return ok;
    }

    // Mongod on win32 defines a value for this function. In all other executables it is NULL.
    void (*reportEventToSystem)(const char *msg) = 0;

    void mongoAbort(const char *msg) {
        if( reportEventToSystem )
            reportEventToSystem(msg);
        severe() << msg;
        ::abort();
    }

    // Returns false when request includes 'end'
    void assembleResponse( OperationContext* txn,
                           Message& m,
                           DbResponse& dbresponse,
                           const HostAndPort& remote ) {

        // before we lock...
        int op = m.operation();
        bool isCommand = false;
        const char *ns = m.singleData()->_data + 4;

        Client& c = cc();
        if (!c.isGod()) {
            c.getAuthorizationSession()->startRequest(txn);

            // We should not be holding any locks at this point
            invariant(!txn->lockState()->isLocked());
        }

        if ( op == dbQuery ) {
            if( strstr(ns, ".$cmd") ) {
                isCommand = true;
                opwrite(m);
                if( strstr(ns, ".$cmd.sys.") ) {
                    if( strstr(ns, "$cmd.sys.inprog") ) {
                        inProgCmd(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.killop") ) {
                        killOp(m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.unlock") ) {
                        unlockFsync(ns, m, dbresponse);
                        return;
                    }
                }
            }
            else {
                opread(m);
            }
        }
        else if( op == dbGetMore ) {
            opread(m);
        }
        else {
            opwrite(m);
        }

        // Increment op counters.
        switch (op) {
        case dbQuery:
            if (!isCommand) {
                globalOpCounters.gotQuery();
            }
            else {
                // Command counting is deferred, since it is not known yet whether the command
                // needs counting.
            }
            break;
        case dbGetMore:
            globalOpCounters.gotGetMore();
            break;
        case dbInsert:
            // Insert counting is deferred, since it is not known yet whether the insert contains
            // multiple documents (each of which needs to be counted).
            break;
        case dbUpdate:
            globalOpCounters.gotUpdate();
            break;
        case dbDelete:
            globalOpCounters.gotDelete();
            break;
        }
        
        scoped_ptr<CurOp> nestedOp;
        CurOp* currentOpP = c.curop();
        if ( currentOpP->active() ) {
            nestedOp.reset( new CurOp( &c , currentOpP ) );
            currentOpP = nestedOp.get();
        }
        else {
            c.newTopLevelRequest();
        }

        CurOp& currentOp = *currentOpP;
        currentOp.reset(remote,op);

        OpDebug& debug = currentOp.debug();
        debug.op = op;

        long long logThreshold = serverGlobalParams.slowMS;
        bool shouldLog = logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1));

        if ( op == dbQuery ) {
            if ( handlePossibleShardedMessage( m , &dbresponse ) )
                return;
            receivedQuery(txn, c , dbresponse, m );
        }
        else if ( op == dbGetMore ) {
            if ( ! receivedGetMore(txn, dbresponse, m, currentOp) )
                shouldLog = true;
        }
        else if ( op == dbMsg ) {
            // deprecated - replaced by commands
            char *p = m.singleData()->_data;
            int len = strlen(p);
            if ( len > 400 )
                log() << curTimeMillis64() % 10000 <<
                      " long msg received, len:" << len << endl;

            Message *resp = new Message();
            if ( strcmp( "end" , p ) == 0 )
                resp->setData( opReply , "dbMsg end no longer supported" );
            else
                resp->setData( opReply , "i am fine - dbMsg deprecated");

            dbresponse.response = resp;
            dbresponse.responseTo = m.header()->id;
        }
        else {
            try {
                const NamespaceString nsString( ns );

                // The following operations all require authorization.
                // dbInsert, dbUpdate and dbDelete can be easily pre-authorized,
                // here, but dbKillCursors cannot.
                if ( op == dbKillCursors ) {
                    currentOp.ensureStarted();
                    logThreshold = 10;
                    receivedKillCursors(txn, m);
                }
                else if ( !nsString.isValid() ) {
                    // Only killCursors doesn't care about namespaces
                    uassert( 16257, str::stream() << "Invalid ns [" << ns << "]", false );
                }
                else if ( op == dbInsert ) {
                    receivedInsert(txn, m, currentOp);
                }
                else if ( op == dbUpdate ) {
                    receivedUpdate(txn, m, currentOp);
                }
                else if ( op == dbDelete ) {
                    receivedDelete(txn, m, currentOp);
                }
                else {
                    mongo::log() << "    operation isn't supported: " << op << endl;
                    currentOp.done();
                    shouldLog = true;
                }
            }
            catch ( UserException& ue ) {
                LOG(3) << " Caught Assertion in " << opToString(op) << ", continuing "
                       << ue.toString() << endl;
                debug.exceptionInfo = ue.getInfo();
            }
            catch ( AssertionException& e ) {
                LOG(3) << " Caught Assertion in " << opToString(op) << ", continuing "
                       << e.toString() << endl;
                debug.exceptionInfo = e.getInfo();
                shouldLog = true;
            }
        }
        currentOp.ensureStarted();
        currentOp.done();
        debug.executionTime = currentOp.totalTimeMillis();

        logThreshold += currentOp.getExpectedLatencyMs();

        if ( shouldLog || debug.executionTime > logThreshold ) {
            LOG(0) << debug.report( currentOp ) << endl;
        }

        if ( currentOp.shouldDBProfile( debug.executionTime ) ) {
            // performance profiling is on
            if (txn->lockState()->hasAnyReadLock()) {
                LOG(1) << "note: not profiling because recursive read lock" << endl;
            }
            else if ( lockedForWriting() ) {
                LOG(1) << "note: not profiling because doing fsync+lock" << endl;
            }
            else {
                profile(txn, c, op, currentOp);
            }
        }

        debug.recordStats();
        debug.reset();
    } /* assembleResponse() */

    void receivedKillCursors(OperationContext* txn, Message& m) {
        int *x = (int *) m.singleData()->_data;
        x++; // reserved
        int n = *x++;

        uassert( 13659 , "sent 0 cursors to kill" , n != 0 );
        massert( 13658 , str::stream() << "bad kill cursors size: " << m.dataSize() , m.dataSize() == 8 + ( 8 * n ) );
        uassert( 13004 , str::stream() << "sent negative cursors to kill: " << n  , n >= 1 );

        if ( n > 2000 ) {
            ( n < 30000 ? warning() : error() ) << "receivedKillCursors, n=" << n << endl;
            verify( n < 30000 );
        }

        int found = CollectionCursorCache::eraseCursorGlobalIfAuthorized(txn, n, (long long *) x);

        if ( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1)) || found != n ) {
            LOG( found == n ? 1 : 0 ) << "killcursors: found " << found << " of " << n << endl;
        }

    }

    /*static*/ 
    void Database::closeDatabase(OperationContext* txn, const string& db) {
        // XXX? - Do we need to close database under global lock or just DB-lock is sufficient ?
        invariant(txn->lockState()->isW());

        Database* database = dbHolder().get(txn, db);
        if ( !database )
            return;

        repl::oplogCheckCloseDatabase(txn, database); // oplog caches some things, dirty its caches

        if( BackgroundOperation::inProgForDb(db) ) {
            log() << "warning: bg op in prog during close db? " << db << endl;
        }

        // Before the files are closed, flush any potentially outstanding changes, which might
        // reference this database. Otherwise we will assert when subsequent commit if needed
        // is called and it happens to have write intents for the removed files.
        txn->recoveryUnit()->commitIfNeeded(true);

        dbHolder().erase(txn, db);
        delete database; // closes files
    }

    void receivedUpdate(OperationContext* txn, Message& m, CurOp& op) {
        DbMessage d(m);
        NamespaceString ns(d.getns());
        uassertStatusOK( userAllowedWriteNS( ns ) );
        op.debug().ns = ns.ns();
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        verify( d.moreJSObjs() );
        verify( query.objsize() < m.header()->dataLen() );
        BSONObj toupdate = d.nextJsObj();
        uassert( 10055 , "update object too large", toupdate.objsize() <= BSONObjMaxUserSize);
        verify( toupdate.objsize() < m.header()->dataLen() );
        verify( query.objsize() + toupdate.objsize() < m.header()->dataLen() );
        bool upsert = flags & UpdateOption_Upsert;
        bool multi = flags & UpdateOption_Multi;
        bool broadcast = flags & UpdateOption_Broadcast;

        Status status = cc().getAuthorizationSession()->checkAuthForUpdate(ns,
                                                                           query,
                                                                           toupdate,
                                                                           upsert);
        audit::logUpdateAuthzCheck(&cc(), ns, query, toupdate, upsert, multi, status.code());
        uassertStatusOK(status);

        op.debug().query = query;
        op.setQuery(query);

        UpdateRequest request(ns);

        request.setUpsert(upsert);
        request.setMulti(multi);
        request.setQuery(query);
        request.setUpdates(toupdate);
        request.setUpdateOpLog(); // TODO: This is wasteful if repl is not active.
        UpdateLifecycleImpl updateLifecycle(broadcast, ns);
        request.setLifecycle(&updateLifecycle);
        UpdateExecutor executor(&request, &op.debug());
        uassertStatusOK(executor.prepare());

        Lock::DBWrite lk(txn->lockState(), ns.ns(), useExperimentalDocLocking);

        // if this ever moves to outside of lock, need to adjust check
        // Client::Context::_finishInit
        if ( ! broadcast && handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx( ns );

        UpdateResult res = executor.execute(txn, ctx.db());

        // for getlasterror
        lastError.getSafe()->recordUpdate( res.existing , res.numMatched , res.upserted );
    }

    void receivedDelete(OperationContext* txn, Message& m, CurOp& op) {
        DbMessage d(m);
        NamespaceString ns(d.getns());
        uassertStatusOK( userAllowedWriteNS( ns ) );

        op.debug().ns = ns.ns();
        int flags = d.pullInt();
        bool justOne = flags & RemoveOption_JustOne;
        bool broadcast = flags & RemoveOption_Broadcast;
        verify( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();

        Status status = cc().getAuthorizationSession()->checkAuthForDelete(ns, pattern);
        audit::logDeleteAuthzCheck(&cc(), ns, pattern, status.code());
        uassertStatusOK(status);

        op.debug().query = pattern;
        op.setQuery(pattern);

        DeleteRequest request(ns);
        request.setQuery(pattern);
        request.setMulti(!justOne);
        request.setUpdateOpLog(true);
        DeleteExecutor executor(&request);
        uassertStatusOK(executor.prepare());
        Lock::DBWrite lk(txn->lockState(), ns.ns());

        // if this ever moves to outside of lock, need to adjust check Client::Context::_finishInit
        if ( ! broadcast && handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx(ns);

        long long n = executor.execute(txn, ctx.db());
        lastError.getSafe()->recordDelete( n );
        op.debug().ndeleted = n;
    }

    QueryResult* emptyMoreResult(long long);

    bool receivedGetMore(OperationContext* txn, DbResponse& dbresponse, Message& m, CurOp& curop ) {
        bool ok = true;

        DbMessage d(m);

        const char *ns = d.getns();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();

        curop.debug().ns = ns;
        curop.debug().ntoreturn = ntoreturn;
        curop.debug().cursorid = cursorid;

        scoped_ptr<AssertionException> ex;
        scoped_ptr<Timer> timer;
        int pass = 0;
        bool exhaust = false;
        QueryResult* msgdata = 0;
        OpTime last;
        while( 1 ) {
            bool isCursorAuthorized = false;
            try {
                const NamespaceString nsString( ns );
                uassert( 16258, str::stream() << "Invalid ns [" << ns << "]", nsString.isValid() );

                Status status = cc().getAuthorizationSession()->checkAuthForGetMore(
                        nsString, cursorid);
                audit::logGetMoreAuthzCheck(&cc(), nsString, cursorid, status.code());
                uassertStatusOK(status);

                if (str::startsWith(ns, "local.oplog.")){
                    while (MONGO_FAIL_POINT(rsStopGetMore)) {
                        sleepmillis(0);
                    }

                    if (pass == 0) {
                        last = getLastSetOptime();
                    }
                    else {
                        repl::waitForOptimeChange(last, 1000/*ms*/);
                    }
                }

                msgdata = newGetMore(txn,
                                     ns,
                                     ntoreturn,
                                     cursorid,
                                     curop,
                                     pass,
                                     exhaust,
                                     &isCursorAuthorized);
            }
            catch ( AssertionException& e ) {
                if ( isCursorAuthorized ) {
                    // If a cursor with id 'cursorid' was authorized, it may have been advanced
                    // before an exception terminated processGetMore.  Erase the ClientCursor
                    // because it may now be out of sync with the client's iteration state.
                    // SERVER-7952
                    // TODO Temporary code, see SERVER-4563 for a cleanup overview.
                    CollectionCursorCache::eraseCursorGlobal(txn, cursorid );
                }
                ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
                ok = false;
                break;
            }
            
            if (msgdata == 0) {
                // this should only happen with QueryOption_AwaitData
                exhaust = false;
                massert(13073, "shutting down", !inShutdown() );
                if ( ! timer ) {
                    timer.reset( new Timer() );
                }
                else {
                    if ( timer->seconds() >= 4 ) {
                        // after about 4 seconds, return. pass stops at 1000 normally.
                        // we want to return occasionally so slave can checkpoint.
                        pass = 10000;
                    }
                }
                pass++;
                if (debug)
                    sleepmillis(20);
                else
                    sleepmillis(2);
                
                // note: the 1100 is beacuse of the waitForDifferent above
                // should eventually clean this up a bit
                curop.setExpectedLatencyMs( 1100 + timer->millis() );
                
                continue;
            }
            break;
        };

        if (ex) {
            BSONObjBuilder err;
            ex->getInfo().append( err );
            BSONObj errObj = err.done();

            curop.debug().exceptionInfo = ex->getInfo();

            replyToQuery(ResultFlag_ErrSet, m, dbresponse, errObj);
            curop.debug().responseLength = dbresponse.response->header()->dataLen();
            curop.debug().nreturned = 1;
            return ok;
        }

        Message *resp = new Message();
        resp->setData(msgdata, true);
        curop.debug().responseLength = resp->header()->dataLen();
        curop.debug().nreturned = msgdata->nReturned;

        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
        
        if( exhaust ) {
            curop.debug().exhaust = true;
            dbresponse.exhaustNS = ns;
        }

        return ok;
    }

    void checkAndInsert(OperationContext* txn,
                        Client::Context& ctx,
                        const char *ns,
                        /*modifies*/BSONObj& js) {

        if ( nsToCollectionSubstring( ns ) == "system.indexes" ) {
            string targetNS = js["ns"].String();
            uassertStatusOK( userAllowedWriteNS( targetNS ) );

            Collection* collection = ctx.db()->getCollection( txn, targetNS );
            if ( !collection ) {
                // implicitly create
                collection = ctx.db()->createCollection( txn, targetNS );
                verify( collection );
            }

            // Only permit interrupting an (index build) insert if the
            // insert comes from a socket client request rather than a
            // parent operation using the client interface.  The parent
            // operation might not support interrupts.
            bool mayInterrupt = txn->getCurOp()->parent() == NULL;

            txn->getCurOp()->setQuery(js);
            Status status = collection->getIndexCatalog()->createIndex(txn, js, mayInterrupt);

            if ( status.code() == ErrorCodes::IndexAlreadyExists )
                return;

            uassertStatusOK( status );
            repl::logOp(txn, "i", ns, js);
            return;
        }

        StatusWith<BSONObj> fixed = fixDocumentForInsert( js );
        uassertStatusOK( fixed.getStatus() );
        if ( !fixed.getValue().isEmpty() )
            js = fixed.getValue();

        Collection* collection = ctx.db()->getCollection( txn, ns );
        if ( !collection ) {
            collection = ctx.db()->createCollection( txn, ns );
            verify( collection );
        }

        StatusWith<DiskLoc> status = collection->insertDocument( txn, js, true );
        uassertStatusOK( status.getStatus() );
        repl::logOp(txn, "i", ns, js);
    }

    NOINLINE_DECL void insertMulti(OperationContext* txn,
                                   Client::Context& ctx,
                                   bool keepGoing,
                                   const char *ns,
                                   vector<BSONObj>& objs,
                                   CurOp& op) {
        size_t i;
        for (i=0; i<objs.size(); i++){
            try {
                checkAndInsert(txn, ctx, ns, objs[i]);
                txn->recoveryUnit()->commitIfNeeded();
            } catch (const UserException&) {
                if (!keepGoing || i == objs.size()-1){
                    globalOpCounters.incInsertInWriteLock(i);
                    throw;
                }
                // otherwise ignore and keep going
            }
        }

        globalOpCounters.incInsertInWriteLock(i);
        op.debug().ninserted = i;
    }

    void receivedInsert(OperationContext* txn, Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        const NamespaceString nsString(ns);
        op.debug().ns = ns;

        uassertStatusOK( userAllowedWriteNS( ns ) );

        if( !d.moreJSObjs() ) {
            // strange.  should we complain?
            return;
        }

        vector<BSONObj> multi;
        while (d.moreJSObjs()){
            BSONObj obj = d.nextJsObj();
            multi.push_back(obj);

            // Check auth for insert (also handles checking if this is an index build and checks
            // for the proper privileges in that case).
            Status status = cc().getAuthorizationSession()->checkAuthForInsert(nsString, obj);
            audit::logInsertAuthzCheck(&cc(), nsString, obj, status.code());
            uassertStatusOK(status);
        }

        Lock::DBWrite lk(txn->lockState(), ns);

        // CONCURRENCY TODO: is being read locked in big log sufficient here?
        // writelock is used to synchronize stepdowns w/ writes
        uassert(10058 , "not master",
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsString.db()));

        if ( handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx(ns);

        if (multi.size() > 1) {
            const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;
            insertMulti(txn, ctx, keepGoing, ns, multi, op);
        } else {
            checkAndInsert(txn, ctx, ns, multi[0]);
            globalOpCounters.incInsertInWriteLock(1);
            op.debug().ninserted = 1;
        }
    }

    /* returns true if there is data on this server.  useful when starting replication.
       local database does NOT count except for rsoplog collection.
       used to set the hasData field on replset heartbeat command response
    */
    bool replHasDatabases(OperationContext* txn) {
        vector<string> names;
        globalStorageEngine->listDatabases( &names );

        if( names.size() >= 2 ) return true;
        if( names.size() == 1 ) {
            if( names[0] != "local" )
                return true;
            // we have a local database.  return true if oplog isn't empty
            {
                Lock::DBRead lk(txn->lockState(), repl::rsoplog);
                BSONObj o;
                if( Helpers::getFirst(txn, repl::rsoplog, o) )
                    return true;
            }
        }
        return false;
    }

    DBDirectClient::DBDirectClient() 
        : _txnOwned(new OperationContextImpl),
          _txn(_txnOwned.get())
    {}

    DBDirectClient::DBDirectClient(OperationContext* txn) 
        : _txn(txn)
    {}

    QueryOptions DBDirectClient::_lookupAvailableOptions() {
        // Exhaust mode is not available in DBDirectClient.
        return QueryOptions(DBClientBase::_lookupAvailableOptions() & ~QueryOption_Exhaust);
    }

namespace {
    class GodScope {
        MONGO_DISALLOW_COPYING(GodScope);
    public:
        GodScope() {
            _prev = cc().setGod(true);
        }
        ~GodScope() { cc().setGod(_prev); }
    private:
        bool _prev;
    };
}  // namespace

    bool DBDirectClient::call( Message &toSend, Message &response, bool assertOk , string * actualServer ) {
        GodScope gs;
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( _txn, toSend, dbResponse , _clientHost );
        verify( dbResponse.response );
        dbResponse.response->concat(); // can get rid of this if we make response handling smarter
        response = *dbResponse.response;
        _txn->recoveryUnit()->commitIfNeeded();
        return true;
    }

    void DBDirectClient::say( Message &toSend, bool isRetry, string * actualServer ) {
        GodScope gs;
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( _txn, toSend, dbResponse , _clientHost );
        _txn->recoveryUnit()->commitIfNeeded();
    }

    auto_ptr<DBClientCursor> DBDirectClient::query(const string &ns, Query query, int nToReturn , int nToSkip ,
            const BSONObj *fieldsToReturn , int queryOptions , int batchSize) {

        //if ( ! query.obj.isEmpty() || nToReturn != 0 || nToSkip != 0 || fieldsToReturn || queryOptions )
        return DBClientBase::query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions , batchSize );
        //
        //verify( query.obj.isEmpty() );
        //throw UserException( (string)"yay:" + ns );
    }

    void DBDirectClient::killCursor( long long id ) {
        // The killCursor command on the DB client is only used by sharding,
        // so no need to have it for MongoD.
        verify(!"killCursor should not be used in MongoD");
    }

    HostAndPort DBDirectClient::_clientHost = HostAndPort( "0.0.0.0" , 0 );

    unsigned long long DBDirectClient::count(const string &ns, const BSONObj& query, int options, int limit, int skip ) {
        if ( skip < 0 ) {
            warning() << "setting negative skip value: " << skip
                << " to zero in query: " << query << endl;
            skip = 0;
        }

        Lock::DBRead lk(_txn->lockState(), ns);
        string errmsg;
        int errCode;
        long long res = runCount( _txn, ns, _countCmd( ns , query , options , limit , skip ) , errmsg, errCode );
        if ( res == -1 ) {
            // namespace doesn't exist
            return 0;
        }
        massert( errCode , str::stream() << "count failed in DBDirectClient: " << errmsg , res >= 0 );
        return (unsigned long long )res;
    }

    DBClientBase* createDirectClient() {
        return new DBDirectClient();
    }


    mongo::mutex exitMutex("exit");
    AtomicUInt numExitCalls = 0;

    bool inShutdown() {
        return numExitCalls > 0;
    }

    static void shutdownServer(OperationContext* txn) {
        // Must hold global lock to get to here
        invariant(txn->lockState()->isW());

        log() << "shutdown: going to close listening sockets..." << endl;
        ListeningSockets::get()->closeAll();

        log() << "shutdown: going to flush diaglog..." << endl;
        _diaglog.flush();

        /* must do this before unmapping mem or you may get a seg fault */
        log() << "shutdown: going to close sockets..." << endl;
        boost::thread close_socket_thread( stdx::bind(MessagingPort::closeAllSockets, 0) );

        // wait until file preallocation finishes
        // we would only hang here if the file_allocator code generates a
        // synchronous signal, which we don't expect
        log() << "shutdown: waiting for fs preallocator..." << endl;
        FileAllocator::get()->waitUntilFinished();

        if (storageGlobalParams.dur) {
            log() << "shutdown: final commit..." << endl;
            getDur().commitNow(txn);

            globalStorageEngine->flushAllFiles(true);
        }

        log() << "shutdown: closing all files..." << endl;
        stringstream ss3;
        MemoryMappedFile::closeAllFiles( ss3 );
        log() << ss3.str() << endl;

        if (storageGlobalParams.dur) {
            dur::journalCleanup(true);
        }

#if !defined(__sunos__)
        if ( lockFile ) {
            log() << "shutdown: removing fs lock..." << endl;
            /* This ought to be an unlink(), but Eliot says the last
               time that was attempted, there was a race condition
               with acquirePathLock().  */
#ifdef _WIN32
            if( _chsize( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << errnoWithDescription(_doserrno) << endl;
            CloseHandle(lockFileHandle);
#else
            if( ftruncate( lockFile , 0 ) )
                log() << "couldn't remove fs lock " << errnoWithDescription() << endl;
            flock( lockFile, LOCK_UN );
#endif
        }
#endif
    }

    void exitCleanly( ExitCode code ) {
        getGlobalEnvironment()->setKillAllOperations();
        if (repl::getGlobalReplicationCoordinator()->getReplicationMode() ==
                repl::ReplicationCoordinator::modeReplSet) {
            repl::theReplSet->shutdown();
        }

        OperationContextImpl txn;
        Lock::GlobalWrite lk(txn.lockState());
        log() << "now exiting" << endl;

        // Execute the graceful shutdown tasks, such as flushing the outstanding journal and data 
        // files, close sockets, etc.
        try {
            shutdownServer(&txn);
        }
        catch (const DBException& ex) {
            severe() << "shutdown failed with DBException " << ex;
            std::terminate();
        }
        catch (const std::exception& ex) {
            severe() << "shutdown failed with std::exception: " << ex.what();
            std::terminate();
        }
        catch (...) {
            severe() << "shutdown failed with exception";
            std::terminate();
        }

        dbexit( code );
    }

    /* not using log() herein in case we are already locked */
    NOINLINE_DECL void dbexit( ExitCode rc, const char *why ) {
        flushForGcov();

        Client * c = currentClient.get();
        audit::logShutdown(c);
        {
            scoped_lock lk( exitMutex );
            if ( numExitCalls++ > 0 ) {
                if ( numExitCalls > 5 ) {
                    // this means something horrible has happened
                    ::_exit( rc );
                }
                log() << "dbexit: " << why << "; exiting immediately";
                if ( c ) c->shutdown();
                ::_exit( rc );
            }
        }

        log() << "dbexit: " << why;

#if defined(_DEBUG)
        try {
            mutexDebugger.programEnding();
        }
        catch (...) { }
#endif

        // block the dur thread from doing any work for the rest of the run
        LOG(2) << "shutdown: groupCommitMutex" << endl;
        SimpleMutex::scoped_lock lk(dur::commitJob.groupCommitMutex);

#ifdef _WIN32
        // Windows Service Controller wants to be told when we are down,
        //  so don't call ::_exit() yet, or say "really exiting now"
        //
        if ( rc == EXIT_WINDOWS_SERVICE_STOP ) {
            if ( c ) c->shutdown();
            return;
        }
#endif
        log() << "dbexit: really exiting now";
        if ( c ) c->shutdown();
        ::_exit(rc);
    }

#if !defined(__sunos__)
    void writePid(int fd) {
        stringstream ss;
        ss << ProcessId::getCurrent() << endl;
        string s = ss.str();
        const char * data = s.c_str();
#ifdef _WIN32
        verify( _write( fd, data, strlen( data ) ) );
#else
        verify( write( fd, data, strlen( data ) ) );
#endif
    }

    void acquirePathLock(bool doingRepair) {
        string name = (boost::filesystem::path(storageGlobalParams.dbpath) / "mongod.lock").string();

        bool oldFile = false;

        if ( boost::filesystem::exists( name ) && boost::filesystem::file_size( name ) > 0 ) {
            oldFile = true;
        }

#ifdef _WIN32
        lockFileHandle = CreateFileA( name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0 /* do not allow anyone else access */, NULL, 
            OPEN_ALWAYS /* success if fh can open */, 0, NULL );

        if (lockFileHandle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            char *msg;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&msg, 0, NULL);
            string m = msg;
            str::stripTrailing(m, "\r\n");
            uasserted( 13627 , str::stream() << "Unable to create/open lock file: " << name << ' ' << m << ". Is a mongod instance already running?" );
        }
        lockFile = _open_osfhandle((intptr_t)lockFileHandle, 0);
#else
        lockFile = open( name.c_str(), O_RDWR | O_CREAT , S_IRWXU | S_IRWXG | S_IRWXO );
        if( lockFile <= 0 ) {
            uasserted( 10309 , str::stream() << "Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?" );
        }
        if (flock( lockFile, LOCK_EX | LOCK_NB ) != 0) {
            close ( lockFile );
            lockFile = 0;
            uassert( 10310 ,  "Unable to lock file: " + name + ". Is a mongod instance already running?",  0 );
        }
#endif

        if ( oldFile ) {
            // we check this here because we want to see if we can get the lock
            // if we can't, then its probably just another mongod running
            
            string errmsg;
            if (doingRepair && dur::haveJournalFiles()) {
                errmsg = "************** \n"
                         "You specified --repair but there are dirty journal files. Please\n"
                         "restart without --repair to allow the journal files to be replayed.\n"
                         "If you wish to repair all databases, please shutdown cleanly and\n"
                         "run with --repair again.\n"
                         "**************";
            }
            else if (storageGlobalParams.dur) {
                if (!dur::haveJournalFiles(/*anyFiles=*/true)) {
                    // Passing anyFiles=true as we are trying to protect against starting in an
                    // unclean state with the journal directory unmounted. If there are any files,
                    // even prealloc files, then it means that it is mounted so we can continue.
                    // Previously there was an issue (SERVER-5056) where we would fail to start up
                    // if killed during prealloc.

                    vector<string> dbnames;
                    globalStorageEngine->listDatabases( &dbnames );

                    if ( dbnames.size() == 0 ) {
                        // this means that mongod crashed
                        // between initial startup and when journaling was initialized
                        // it is safe to continue
                    }
                    else {
                        errmsg = str::stream()
                            << "************** \n"
                            << "old lock file: " << name << ".  probably means unclean shutdown,\n"
                            << "but there are no journal files to recover.\n"
                            << "this is likely human error or filesystem corruption.\n"
                            << "please make sure that your journal directory is mounted.\n"
                            << "found " << dbnames.size() << " dbs.\n"
                            << "see: http://dochub.mongodb.org/core/repair for more information\n"
                            << "*************";
                    }

                }
            }
            else {
                if (!dur::haveJournalFiles() && !doingRepair) {
                    errmsg = str::stream()
                             << "************** \n"
                             << "Unclean shutdown detected.\n"
                             << "Please visit http://dochub.mongodb.org/core/repair for recovery instructions.\n"
                             << "*************";
                }
            }

            if (!errmsg.empty()) {
                cout << errmsg << endl;
#ifdef _WIN32
                CloseHandle( lockFileHandle );
#else
                close ( lockFile );
#endif
                lockFile = 0;
                uassert( 12596 , "old lock file" , 0 );
            }
        }

        // Not related to lock file, but this is where we handle unclean shutdown
        if (!storageGlobalParams.dur && dur::haveJournalFiles()) {
            cout << "**************" << endl;
            cout << "Error: journal files are present in journal directory, yet starting without journaling enabled." << endl;
            cout << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            cout << "**************" << endl;
            uasserted(13597, "can't start without --journal enabled when journal/ files are present");
        }

#ifdef _WIN32
        uassert( 13625, "Unable to truncate lock file", _chsize(lockFile, 0) == 0);
        writePid( lockFile );
        _commit( lockFile );
#else
        uassert( 13342, "Unable to truncate lock file", ftruncate(lockFile, 0) == 0);
        writePid( lockFile );
        fsync( lockFile );
        flushMyDirectory(name);
#endif
    }
#else
    void acquirePathLock(bool) {
        // TODO - this is very bad that the code above not running here.

        // Not related to lock file, but this is where we handle unclean shutdown
        if (!storageGlobalParams.dur && dur::haveJournalFiles()) {
            cout << "**************" << endl;
            cout << "Error: journal files are present in journal directory, yet starting without --journal enabled." << endl;
            cout << "It is recommended that you start with journaling enabled so that recovery may occur." << endl;
            cout << "Alternatively (not recommended), you can backup everything, then delete the journal files, and run --repair" << endl;
            cout << "**************" << endl;
            uasserted(13618, "can't start without --journal enabled when journal/ files are present");
        }
    }
#endif

    // ----- BEGIN Diaglog -----
    DiagLog::DiagLog() : f(0) , level(0), mutex("DiagLog") { 
    }

    void DiagLog::openFile() {
        verify( f == 0 );
        stringstream ss;
        ss << storageGlobalParams.dbpath << "/diaglog." << hex << time(0);
        string name = ss.str();
        f = new ofstream(name.c_str(), ios::out | ios::binary);
        if ( ! f->good() ) {
            log() << "diagLogging couldn't open " << name << endl;
            // todo what is this? :
            throw 1717;
        }
        else {
            log() << "diagLogging using file " << name << endl;
        }
    }

    int DiagLog::setLevel( int newLevel ) {
        scoped_lock lk(mutex);
        int old = level;
        log() << "diagLogging level=" << newLevel << endl;
        if( f == 0 ) { 
            openFile();
        }
        level = newLevel; // must be done AFTER f is set
        return old;
    }
    
    void DiagLog::flush() {
        if ( level ) {
            log() << "flushing diag log" << endl;
            scoped_lock lk(mutex);
            f->flush();
        }
    }
    
    void DiagLog::writeop(char *data,int len) {
        if ( level & 1 ) {
            scoped_lock lk(mutex);
            f->write(data,len);
        }
    }
    
    void DiagLog::readop(char *data, int len) {
        if ( level & 2 ) {
            bool log = (level & 4) == 0;
            OCCASIONALLY log = true;
            if ( log ) {
                scoped_lock lk(mutex);
                verify( f );
                f->write(data,len);
            }
        }
    }

    DiagLog _diaglog;

    // ----- END Diaglog -----

} // namespace mongo
