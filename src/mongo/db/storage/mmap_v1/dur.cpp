// @file dur.cpp durability in the storage engine (crash-safeness / journaling)

/**
*    Copyright (C) 2009 10gen Inc.
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

/*
   phases:

     PREPLOGBUFFER
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
     WRITETOJOURNAL
       we could be unlocked (the main db lock that is...) for this, with sufficient care, but there is some complexity
         have to handle falling behind which would use too much ram (going back into a read lock would suffice to stop that).
         for now (1.7.5/1.8.0) we are in read lock which is not ideal.
     WRITETODATAFILES
       actually write to the database data files in this phase.  currently done by memcpy'ing the writes back to 
       the non-private MMF.  alternatively one could write to the files the traditional way; however the way our 
       storage engine works that isn't any faster (actually measured a tiny bit slower).
     REMAPPRIVATEVIEW
       we could in a write lock quickly flip readers back to the main view, then stay in read lock and do our real
         remapping. with many files (e.g., 1000), remapping could be time consuming (several ms), so we don't want
         to be too frequent.
       there could be a slow down immediately after remapping as fresh copy-on-writes for commonly written pages will
         be required.  so doing these remaps fractionally is helpful. 

   mutexes:

     READLOCK dbMutex (big 'R')
     LOCK groupCommitMutex
       PREPLOGBUFFER()
     READLOCK mmmutex
       commitJob.reset()
     UNLOCK dbMutex                      // now other threads can write
       WRITETOJOURNAL()
       WRITETODATAFILES()
     UNLOCK mmmutex
     UNLOCK groupCommitMutex

   every Nth groupCommit, at the end, we REMAPPRIVATEVIEW() at the end of the work. because of
   that we are in W lock for that groupCommit, which is nonideal of course.

   @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#include "mongo/platform/basic.h"

#include <boost/thread/thread.hpp>

#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/dur_commitjob.h"
#include "mongo/db/storage/mmap_v1/dur_journal.h"
#include "mongo/db/storage/mmap_v1/dur_recover.h"
#include "mongo/db/storage/mmap_v1/dur_stats.h"
#include "mongo/db/storage_options.h"
#include "mongo/server.h"
#include "mongo/util/concurrency/race.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/timer.h"

using namespace mongoutils;

namespace mongo {

    MONGO_LOG_DEFAULT_TAG_FILE(::mongo::logger::LogTag::kStorage);

    namespace dur {

        void PREPLOGBUFFER(JSectHeader& outParm, AlignedBuilder&);
        void WRITETOJOURNAL(JSectHeader h, AlignedBuilder& uncompressed);
        void WRITETODATAFILES(const JSectHeader& h, AlignedBuilder& uncompressed);

        /** declared later in this file
            only used in this file -- use DurableInterface::commitNow() outside
        */
        static void groupCommit(OperationContext* txn, Lock::GlobalWrite *lgw);

        CommitJob& commitJob = *(new CommitJob()); // don't destroy

        Stats stats;

        void Stats::S::reset() {
            memset(this, 0, sizeof(*this));
        }

        Stats::Stats() {
            _a.reset();
            _b.reset();
            curr = &_a;
            _intervalMicros = 3000000;
        }

        Stats::S * Stats::other() {
            return curr == &_a ? &_b : &_a;
        }
        string _CSVHeader();

        string Stats::S::_CSVHeader() { 
            return "cmts  jrnMB\twrDFMB\tcIWLk\tearly\tprpLgB  wrToJ\twrToDF\trmpPrVw";
        }

        string Stats::S::_asCSV() { 
            stringstream ss;
            ss << 
                setprecision(2) << 
                _commits << '\t' << fixed << 
                _journaledBytes / 1000000.0 << '\t' << 
                _writeToDataFilesBytes / 1000000.0 << '\t' << 
                _commitsInWriteLock << '\t' << 
                _earlyCommits <<  '\t' << 
                (unsigned) (_prepLogBufferMicros/1000) << '\t' << 
                (unsigned) (_writeToJournalMicros/1000) << '\t' << 
                (unsigned) (_writeToDataFilesMicros/1000) << '\t' << 
                (unsigned) (_remapPrivateViewMicros/1000);
            return ss.str();
        }

        //int getAgeOutJournalFiles();
        BSONObj Stats::S::_asObj() {
            BSONObjBuilder b;
            b << 
                       "commits" << _commits <<
                       "journaledMB" << _journaledBytes / 1000000.0 <<
                       "writeToDataFilesMB" << _writeToDataFilesBytes / 1000000.0 <<
                       "compression" << _journaledBytes / (_uncompressedBytes+1.0) <<
                       "commitsInWriteLock" << _commitsInWriteLock <<
                       "earlyCommits" << _earlyCommits << 
                       "timeMs" <<
                       BSON( "dt" << _dtMillis <<
                             "prepLogBuffer" << (unsigned) (_prepLogBufferMicros/1000) <<
                             "writeToJournal" << (unsigned) (_writeToJournalMicros/1000) <<
                             "writeToDataFiles" << (unsigned) (_writeToDataFilesMicros/1000) <<
                             "remapPrivateView" << (unsigned) (_remapPrivateViewMicros/1000)
                           );
            if (storageGlobalParams.journalCommitInterval != 0)
                b << "journalCommitIntervalMs" << storageGlobalParams.journalCommitInterval;
            return b.obj();
        }

        BSONObj Stats::asObj() {
            return other()->_asObj();
        }

        void Stats::rotate() {
            unsigned long long now = curTimeMicros64();
            unsigned long long dt = now - _lastRotate;
            if( dt >= _intervalMicros && _intervalMicros ) {
                // rotate
                curr->_dtMillis = (unsigned) (dt/1000);
                _lastRotate = now;
                curr = other();
                curr->reset();
            }
        }

        void* NonDurableImpl::writingPtr(void *x, unsigned len) { 
            return x; 
        }

        void NonDurableImpl::declareWriteIntent(void *, unsigned) { 
        }

        bool NonDurableImpl::commitNow(OperationContext* txn) {
            cc().checkpointHappened();   // XXX: remove when all dur goes through DurRecoveryUnit
            return false;
        }

        bool NonDurableImpl::commitIfNeeded(OperationContext* txn, bool force) {
            cc().checkpointHappened();   // XXX: remove when all dur goes through DurRecoveryUnit
            return false;
        }



        static DurableImpl* durableImpl = new DurableImpl();
        static NonDurableImpl* nonDurableImpl = new NonDurableImpl();
        DurableInterface* DurableInterface::_impl = nonDurableImpl;

        void DurableInterface::enableDurability() {
            verify(_impl == nonDurableImpl);
            _impl = durableImpl;
        }

        void DurableInterface::disableDurability() {
            verify(_impl == durableImpl);
            massert(13616, "can't disable durability with pending writes", !commitJob.hasWritten());
            _impl = nonDurableImpl;
        }

        bool DurableImpl::commitNow(OperationContext* txn) {
            stats.curr->_earlyCommits++;
            groupCommit(txn, NULL);
            cc().checkpointHappened();
            return true;
        }

        bool DurableImpl::awaitCommit() {
            commitJob._notify.awaitBeyondNow();
            return true;
        }

        /** Declare that a file has been created
            Normally writes are applied only after journaling, for safety.  But here the file
            is created first, and the journal will just replay the creation if the create didn't
            happen because of crashing.
        */
        void DurableImpl::createdFile(const std::string& filename, unsigned long long len) {
            shared_ptr<DurOp> op( new FileCreatedOp(filename, len) );
            commitJob.noteOp(op);
        }

        void* DurableImpl::writingPtr(void *x, unsigned len) {
            void *p = x;
            declareWriteIntent(p, len);
            return p;
        }

        /** declare intent to write
            @param ofs offset within buf at which we will write
            @param len the length at ofs we will write
            @return new buffer pointer.
        */
        void* DurableImpl::writingAtOffset(void *buf, unsigned ofs, unsigned len) {
            char *p = (char *) buf;
            declareWriteIntent(p+ofs, len);
            return p;
        }

        void* DurableImpl::writingRangesAtOffsets(void *buf, const vector< pair< long long, unsigned > > &ranges ) {
            char *p = (char *) buf;
            for( vector< pair< long long, unsigned > >::const_iterator i = ranges.begin();
                    i != ranges.end(); ++i ) {
                declareWriteIntent( p + i->first, i->second );
            }
            return p;
        }

        bool DurableImpl::isCommitNeeded() const {
            return commitJob.bytes() > UncommittedBytesLimit;
        }

        bool NOINLINE_DECL DurableImpl::_aCommitIsNeeded(OperationContext* txn) {
            switch (txn->lockState()->threadState()) {
                case '\0': {
                    // lock_w() can call in this state at times if a commit is needed before attempting 
                    // its lock.
                    Lock::GlobalRead r(txn->lockState());
                    if( commitJob.bytes() < UncommittedBytesLimit ) {
                        // someone else beat us to it
                        //
                        // note before of 'R' state, many threads can pile-in to this point and 
                        // still fall through to below, and they will exit without doing work later
                        // once inside groupCommitMutex.  this is all likely inefficient.  maybe 
                        // groupCommitMutex should be on top.
                        return false;
                    }
                    commitNow(txn);
                    return true;
                }
                case 'w': {
                    if (txn->lockState()->isAtLeastReadLocked("local")) {
                        LOG(2) << "can't commitNow from commitIfNeeded, as we are in local db lock";
                        return false;
                    }
                    if (txn->lockState()->isAtLeastReadLocked("admin")) {
                        LOG(2) << "can't commitNow from commitIfNeeded, as we are in admin db lock";
                        return false;
                    }

                    LOG(1) << "commitIfNeeded upgrading from shared write to exclusive write state"
                           << endl;
                    Lock::UpgradeGlobalLockToExclusive ex(txn->lockState());
                    if (ex.gotUpgrade()) {
                        commitNow(txn);
                    }
                    return true;
                }

                case 'W':
                case 'R':
                    commitNow(txn);
                    return true;

                case 'r':
                    return false;

                default:
                    fassertFailed(16434); // unknown lock type
            }
        }

        /** we may need to commit earlier than normal if data are being written at 
            very high rates. 

            note you can call this unlocked, and that is a good idea as if you are in 
            say, a 'w' lock state, we can't do the commit

            @param force force a commit now even if seemingly not needed - ie the caller may 
            know something we don't such as that files will be closed

            perf note: this function is called a lot, on every lock_w() ... and usually returns right away
        */
        bool DurableImpl::commitIfNeeded(OperationContext* txn, bool force) {
            // this is safe since since conceptually if you call commitIfNeeded, we're at a valid
            // spot in an operation to be terminated.
            cc().checkpointHappened();

            if( likely( commitJob.bytes() < UncommittedBytesLimit && !force ) ) {
                return false;
            }
            return _aCommitIsNeeded(txn);
        }

        /** Used in _DEBUG builds to check that we didn't overwrite the last intent
            that was declared.  called just before writelock release.  we check a few
            bytes after the declared region to see if they changed.

            @see MongoMutex::_releasedWriteLock

            SLOW
        */
#if 0
        void DurableImpl::debugCheckLastDeclaredWrite() {
            static int n;
            ++n;

            verify(debug && storageGlobalParams.dur);
            if (commitJob.writes().empty())
                return;
            const WriteIntent &i = commitJob.lastWrite();
            size_t ofs;
            DurableMappedFile *mmf = privateViews.find(i.start(), ofs);
            if( mmf == 0 )
                return;
            size_t past = ofs + i.length();
            if( mmf->length() < past + 8 )
                return; // too close to end of view
            char *priv = (char *) mmf->getView();
            char *writ = (char *) mmf->view_write();
            unsigned long long *a = (unsigned long long *) (priv+past);
            unsigned long long *b = (unsigned long long *) (writ+past);
            if( *a != *b ) {
                for( set<WriteIntent>::iterator it(commitJob.writes().begin()), end((commitJob.writes().begin())); it != end; ++it ) {
                    const WriteIntent& wi = *it;
                    char *r1 = (char*) wi.start();
                    char *r2 = (char*) wi.end();
                    if( r1 <= (((char*)a)+8) && r2 > (char*)a ) {
                        //log() << "it's ok " << wi.p << ' ' << wi.len << endl;
                        return;
                    }
                }
                log() << "journal data after write area " << i.start() << " does not agree" << endl;
                log() << " was:  " << ((void*)b) << "  " << hexdump((char*)b, 8) << endl;
                log() << " now:  " << ((void*)a) << "  " << hexdump((char*)a, 8) << endl;
                log() << " n:    " << n << endl;
                log() << endl;
            }
        }
#endif

        // Functor to be called over all MongoFiles

        class validateSingleMapMatches {
        public:
            validateSingleMapMatches(unsigned long long& bytes) :_bytes(bytes)  {}
            void operator () (MongoFile *mf) {
                if( mf->isDurableMappedFile() ) {
                    DurableMappedFile *mmf = (DurableMappedFile*) mf;
                    const unsigned char *p = (const unsigned char *) mmf->getView();
                    const unsigned char *w = (const unsigned char *) mmf->view_write();

                    if (!p || !w) return; // File not fully opened yet

                    _bytes += mmf->length();

                    verify( mmf->length() == (unsigned) mmf->length() );

                    if (memcmp(p, w, (unsigned) mmf->length()) == 0)
                        return; // next file

                    unsigned low = 0xffffffff;
                    unsigned high = 0;
                    log() << "DurParanoid mismatch in " << mmf->filename() << endl;
                    int logged = 0;
                    unsigned lastMismatch = 0xffffffff;
                    for( unsigned i = 0; i < mmf->length(); i++ ) {
                        if( p[i] != w[i] ) {
                            if( lastMismatch != 0xffffffff && lastMismatch+1 != i )
                                log() << endl; // separate blocks of mismatches
                            lastMismatch= i;
                            if( ++logged < 60 ) {
                                if( logged == 1 )
                                    log() << "ofs % 628 = 0x" << hex << (i%628) << endl; // for .ns files to find offset in record
                                stringstream ss;
                                ss << "mismatch ofs:" << hex << i <<  "\tfilemap:" << setw(2) << (unsigned) w[i] << "\tprivmap:" << setw(2) << (unsigned) p[i];
                                if( p[i] > 32 && p[i] <= 126 )
                                    ss << '\t' << p[i];
                                log() << ss.str() << endl;
                            }
                            if( logged == 60 )
                                log() << "..." << endl;
                            if( i < low ) low = i;
                            if( i > high ) high = i;
                        }
                    }
                    if( low != 0xffffffff ) {
                        std::stringstream ss;
                        ss << "journal error warning views mismatch " << mmf->filename() << ' ' << (hex) << low << ".." << high << " len:" << high-low+1;
                        log() << ss.str() << endl;
                        log() << "priv loc: " << (void*)(p+low) << ' ' << endl;
                        //vector<WriteIntent>& _intents = commitJob.wi()._intents;
                        //(void) _intents; // mark as unused. Useful for inspection in debugger

                        // should we abort() here so this isn't unnoticed in some circumstances?
                        massert(13599, "Written data does not match in-memory view. Missing WriteIntent?", false);
                    }
                }
            }
        private:
            unsigned long long& _bytes;
        };

        /** (SLOW) diagnostic to check that the private view and the non-private view are in sync.
        */
        void debugValidateAllMapsMatch() {
            if (!(storageGlobalParams.durOptions & StorageGlobalParams::DurParanoid))
                return;

            unsigned long long bytes = 0;
            Timer t;
            MongoFile::forEach(validateSingleMapMatches(bytes));
            OCCASIONALLY log() << "DurParanoid map check " << t.millis() << "ms for " <<  (bytes / (1024*1024)) << "MB" << endl;
        }

        extern size_t privateMapBytes;

        static void _REMAPPRIVATEVIEW(OperationContext* txn) {
            // todo: Consider using ProcessInfo herein and watching for getResidentSize to drop.  that could be a way 
            //       to assure very good behavior here.

            static unsigned startAt;
            static unsigned long long lastRemap;

            LOG(4) << "journal REMAPPRIVATEVIEW" << endl;

            invariant(txn->lockState()->isW());
            invariant(!commitJob.hasWritten());

            // we want to remap all private views about every 2 seconds.  there could be ~1000 views so
            // we do a little each pass; beyond the remap time, more significantly, there will be copy on write
            // faults after remapping, so doing a little bit at a time will avoid big load spikes on
            // remapping.
            unsigned long long now = curTimeMicros64();
            double fraction = (now-lastRemap)/2000000.0;
            if (storageGlobalParams.durOptions & StorageGlobalParams::DurAlwaysRemap)
                fraction = 1;
            lastRemap = now;

#if defined(_WIN32) || defined(__sunos__)
            // Note that this negatively affects performance.
            // We must grab the exclusive lock here because remapPrivateView() on Windows and
            // Solaris need to grab it as well, due to the lack of an atomic way to remap a
            // memory mapped file.
            // See SERVER-5723 for performance improvement.
            // See SERVER-5680 to see why this code is necessary on Windows.
            // See SERVER-8795 to see why this code is necessary on Solaris.
            LockMongoFilesExclusive lk;
#else
            LockMongoFilesShared lk;
#endif
            set<MongoFile*>& files = MongoFile::getAllFiles();
            unsigned sz = files.size();
            if( sz == 0 )
                return;

            {
                // be careful not to use too much memory if the write rate is 
                // extremely high
                double f = privateMapBytes / ((double)UncommittedBytesLimit);
                if( f > fraction ) { 
                    fraction = f;
                }
                privateMapBytes = 0;
            }

            unsigned ntodo = (unsigned) (sz * fraction);
            if( ntodo < 1 ) ntodo = 1;
            if( ntodo > sz ) ntodo = sz;

            const set<MongoFile*>::iterator b = files.begin();
            const set<MongoFile*>::iterator e = files.end();
            set<MongoFile*>::iterator i = b;
            // skip to our starting position
            for( unsigned x = 0; x < startAt; x++ ) {
                i++;
                if( i == e ) i = b;
            }
            unsigned startedAt = startAt;
            startAt = (startAt + ntodo) % sz; // mark where to start next time

            Timer t;
            for( unsigned x = 0; x < ntodo; x++ ) {
                dassert( i != e );
                if( (*i)->isDurableMappedFile() ) {
                    DurableMappedFile *mmf = (DurableMappedFile*) *i;
                    verify(mmf);
                    if( mmf->willNeedRemap() ) {
                        mmf->remapThePrivateView();
                    }
                    i++;
                    if( i == e ) i = b;
                }
            }
            LOG(2) << "journal REMAPPRIVATEVIEW done startedAt: " << startedAt << " n:" << ntodo << ' ' << t.millis() << "ms" << endl;
        }

        /** We need to remap the private views periodically. otherwise they would become very large.
            Call within write lock.  See top of file for more commentary.
        */
        static void REMAPPRIVATEVIEW(OperationContext* txn) {
            Timer t;
            _REMAPPRIVATEVIEW(txn);
            stats.curr->_remapPrivateViewMicros += t.micros();
        }

        // this is a pseudo-local variable in the groupcommit functions 
        // below.  however we don't truly do that so that we don't have to 
        // reallocate, and more importantly regrow it, on every single commit.
        static AlignedBuilder __theBuilder(4 * 1024 * 1024);

        static bool _groupCommitWithLimitedLocks(OperationContext* txn) {
            AlignedBuilder &ab = __theBuilder;

            invariant(!txn->lockState()->isLocked());

            // do we need this to be greedy, so that it can start working fairly soon?
            // probably: as this is a read lock, it wouldn't change anything if only reads anyway.
            // also needs to stop greed. our time to work before clearing lk1 is not too bad, so 
            // not super critical, but likely 'correct'.  todo.
            scoped_ptr<Lock::GlobalRead> lk1(new Lock::GlobalRead(txn->lockState()));

            SimpleMutex::scoped_lock lk2(commitJob.groupCommitMutex);

            commitJob.commitingBegin(); // increments the commit epoch for getlasterror j:true

            if( !commitJob.hasWritten() ) {
                // getlasterror request could have came after the data was already committed
                commitJob.committingNotifyCommitted();
                return true;
            }

            JSectHeader h;
            // need to be in readlock (writes excluded) for this as write intent stuctures point into 
            // the private mmap for their actual data.  i suppose we could lock individual databases 
            // and do them one at a time or in parallel (surely the latter would make sense if one went 
            // that route...)
            PREPLOGBUFFER(h,ab); 

            LockMongoFilesShared lk3;

            unsigned abLen = ab.len();
            commitJob.committingReset(); // must be reset before allowing anyone to write
            DEV verify( !commitJob.hasWritten() );

            // release the readlock -- allowing others to now write while we are writing to the journal (etc.)
            lk1.reset();

            // ****** now other threads can do writes ******

            WRITETOJOURNAL(h, ab);
            verify( abLen == ab.len() ); // a check that no one touched the builder while we were doing work. if so, our locking is wrong.

            // data is now in the journal, which is sufficient for acknowledging getLastError.
            // (ok to crash after that)
            commitJob.committingNotifyCommitted();

            // note the higher-up-the-chain locking of filesLockedFsync is important here, 
            // as we are not in Lock::GlobalRead anymore. private view readers won't see 
            // anything as we do this, but external viewers of the datafiles will see them 
            // mutating.
            WRITETODATAFILES(h, ab);
            verify( abLen == ab.len() ); // check again wasn't modded
            ab.reset();

            // can't : d.dbMutex._remapPrivateViewRequested = true;
            // (writes have happened we released)

            return true;
        }

        /** @return true if committed; false if lock acquisition timed out (we only try for a read lock herein and only wait for a certain duration). */
        static bool groupCommitWithLimitedLocks(OperationContext* txn) {
            try {
                return _groupCommitWithLimitedLocks(txn);
            }
            catch(DBException& e ) {
                log() << "dbexception in groupCommitLL causing immediate shutdown: " << e.toString() << endl;
                mongoAbort("dur1");
            }
            catch(std::ios_base::failure& e) {
                log() << "ios_base exception in groupCommitLL causing immediate shutdown: " << e.what() << endl;
                mongoAbort("dur2");
            }
            catch(std::bad_alloc& e) {
                log() << "bad_alloc exception in groupCommitLL causing immediate shutdown: " << e.what() << endl;
                mongoAbort("dur3");
            }
            catch(std::exception& e) {
                log() << "exception in dur::groupCommitLL causing immediate shutdown: " << e.what() << endl;
                mongoAbort("dur4");
            }
            return false;
        }

        static void _groupCommit(OperationContext* txn, Lock::GlobalWrite *lgw) {
            LOG(4) << "_groupCommit " << endl;

            // We are 'R' or 'W'
            invariant(txn->lockState()->isLockedForCommitting());

            {
                AlignedBuilder &ab = __theBuilder;

                // we need to make sure two group commits aren't running at the same time
                // (and we are only read locked in the dbMutex, so it could happen -- while 
                // there is only one dur thread, "early commits" can be done by other threads)
                SimpleMutex::scoped_lock lk(commitJob.groupCommitMutex);

                commitJob.commitingBegin();

                if( !commitJob.hasWritten() ) {
                    // getlasterror request could have came after the data was already committed
                    commitJob.committingNotifyCommitted();
                }
                else {
                    JSectHeader h;
                    PREPLOGBUFFER(h,ab);

                    // todo : write to the journal outside locks, as this write can be slow.
                    //        however, be careful then about remapprivateview as that cannot be done 
                    //        if new writes are then pending in the private maps.
                    WRITETOJOURNAL(h, ab);

                    // data is now in the journal, which is sufficient for acknowledging getLastError.
                    // (ok to crash after that)
                    commitJob.committingNotifyCommitted();

                    WRITETODATAFILES(h, ab);
                    debugValidateAllMapsMatch();

                    commitJob.committingReset();
                    ab.reset();
                }
            }

            // REMAPPRIVATEVIEW
            //
            // remapping private views must occur after WRITETODATAFILES otherwise
            // we wouldn't see newly written data on reads.
            //
            DEV verify( !commitJob.hasWritten() );
            if (!txn->lockState()->isW()) {
                // todo: note we end up here i believe if our lock state is X -- and that might not be what we want.

                // REMAPPRIVATEVIEW needs done in a write lock (as there is a short window during remapping when each view 
                // might not exist) thus we do it later.
                // 
                // if commitIfNeeded() operations are not in a W lock, you could get too big of a private map 
                // on a giant operation.  for now they will all be W.
                // 
                // If desired, perhaps this can be eliminated on posix as it may be that the remap is race-free there.
                //
                // For durthread, lgw is set, and we can upgrade to a W lock for the remap. we do this way as we don't want 
                // to be in W the entire time we were committing about (in particular for WRITETOJOURNAL() which takes time).
                if( lgw ) { 
                    LOG(4) << "_groupCommit upgrade" << endl;
                    lgw->upgrade();
                    REMAPPRIVATEVIEW(txn);
                }
            }
            else {
                stats.curr->_commitsInWriteLock++;
                // however, if we are already write locked, we must do it now -- up the call tree someone
                // may do a write without a new lock acquisition.  this can happen when DurableMappedFile::close() calls
                // this method when a file (and its views) is about to go away.
                //
                REMAPPRIVATEVIEW(txn);
            }
        }

        /** locking: in at least 'R' when called
                     or, for early commits (commitIfNeeded), in W or X
            @param lwg set if the durcommitthread *only* -- then we will upgrade the lock 
                   to W so we can remapprivateview. only durcommitthread calls with 
                   lgw != 0 as more than one thread upgrading would deadlock
            @see DurableMappedFile::close()
        */
        static void groupCommit(OperationContext* txn, Lock::GlobalWrite *lgw) {
            try {
                _groupCommit(txn, lgw);
            }
            catch(DBException& e ) { 
                log() << "dbexception in groupCommit causing immediate shutdown: " << e.toString() << endl;
                mongoAbort("gc1");
            }
            catch(std::ios_base::failure& e) { 
                log() << "ios_base exception in groupCommit causing immediate shutdown: " << e.what() << endl;
                mongoAbort("gc2");
            }
            catch(std::bad_alloc& e) { 
                log() << "bad_alloc exception in groupCommit causing immediate shutdown: " << e.what() << endl;
                mongoAbort("gc3");
            }
            catch(std::exception& e) {
                log() << "exception in dur::groupCommit causing immediate shutdown: " << e.what() << endl;
                mongoAbort("gc4");
            }
            LOG(4) << "groupCommit end" << endl;
        }

        static void durThreadGroupCommit() {
            OperationContextImpl txn;
            SimpleMutex::scoped_lock flk(filesLockedFsync);

            const int N = 10;
            static int n;
            if (privateMapBytes < UncommittedBytesLimit && ++n % N &&
                (storageGlobalParams.durOptions &
                 StorageGlobalParams::DurAlwaysRemap) == 0) {
                // limited locks version doesn't do any remapprivateview at all, so only try this if privateMapBytes
                // is in an acceptable range.  also every Nth commit, we do everything so we can do some remapping;
                // remapping a lot all at once could cause jitter from a large amount of copy-on-writes all at once.
                if( groupCommitWithLimitedLocks(&txn) )
                    return;
            }

            // we get a write lock, downgrade, do work, upgrade, finish work.
            // getting a write lock is helpful also as we need to be greedy and not be starved here
            // note our "stopgreed" parm -- to stop greed by others while we are working. you can't write 
            // anytime soon anyway if we are journaling for a while, that was the idea.
            Lock::GlobalWrite w(txn.lockState());
            w.downgrade();
            groupCommit(&txn, &w);
        }

        /** called when a DurableMappedFile is closing -- we need to go ahead and group commit in that case before its
            views disappear
        */
        void closingFileNotification() {
            if (!storageGlobalParams.dur)
                return;

            if (commitJob.hasWritten()) {
                if (inShutdown()) {
                    log() << "journal warning files are closing outside locks with writes pending"
                          << endl;
                }
                else {
                    fassert(18507, !"File is closing while there are unwritten changes.");
                }
            }
        }

        extern int groupCommitIntervalMs;
        boost::filesystem::path getJournalDir();

        void durThread() {
            Client::initThread("journal");

            bool samePartition = true;
            try {
                const std::string dbpathDir =
                    boost::filesystem::path(storageGlobalParams.dbpath).string();
                samePartition = onSamePartition(getJournalDir().string(), dbpathDir);
            }
            catch(...) {
            }

            while( !inShutdown() ) {
                RACECHECK

                unsigned ms = storageGlobalParams.journalCommitInterval;
                if( ms == 0 ) { 
                    // use default
                    ms = samePartition ? 100 : 30;
                }

                unsigned oneThird = (ms / 3) + 1; // +1 so never zero

                try {
                    stats.rotate();

                    // commit sooner if one or more getLastError j:true is pending
                    sleepmillis(oneThird);
                    for( unsigned i = 1; i <= 2; i++ ) {
                        if( commitJob._notify.nWaiting() )
                            break;
                        if( commitJob.bytes() > UncommittedBytesLimit / 2  )
                            break;
                        sleepmillis(oneThird);
                    }
                                        
                    //DEV log() << "privateMapBytes=" << privateMapBytes << endl;

                    durThreadGroupCommit();
                }
                catch(std::exception& e) {
                    log() << "exception in durThread causing immediate shutdown: " << e.what() << endl;
                    mongoAbort("exception in durThread");
                }
            }
            cc().shutdown();
        }

        void recover(OperationContext* txn);
        void preallocateFiles();

        /** at startup, recover, and then start the journal threads */
        void startup() {
            if (!storageGlobalParams.dur)
                return;

#if defined(_DURABLEDEFAULTON)
            DEV { 
                if( time(0) & 1 ) {
                    storageGlobalParams.durOptions |= StorageGlobalParams::DurAlwaysCommit;
                    log() << "_DEBUG _DURABLEDEFAULTON : forcing DurAlwaysCommit mode for this run" << endl;
                }
                if( time(0) & 2 ) {
                    storageGlobalParams.durOptions |= StorageGlobalParams::DurAlwaysRemap;
                    log() << "_DEBUG _DURABLEDEFAULTON : forcing DurAlwaysRemap mode for this run" << endl;
                }
            }
#endif

            DurableInterface::enableDurability();

            journalMakeDir();

            OperationContextImpl txn;
            try {
                recover(&txn);
            }
            catch(DBException& e) {
                log() << "dbexception during recovery: " << e.toString() << endl;
                throw;
            }
            catch(std::exception& e) {
                log() << "std::exception during recovery: " << e.what() << endl;
                throw;
            }
            catch(...) {
                log() << "exception during recovery" << endl;
                throw;
            }

            preallocateFiles();

            boost::thread t(durThread);
        }

        void DurableImpl::syncDataAndTruncateJournal(OperationContext* txn) {
            invariant(txn->lockState()->isW());

            // a commit from the commit thread won't begin while we are in the write lock,
            // but it may already be in progress and the end of that work is done outside 
            // (dbMutex) locks. This line waits for that to complete if already underway.
            {
                SimpleMutex::scoped_lock lk(commitJob.groupCommitMutex);
            }

            commitNow(txn);
            globalStorageEngine->flushAllFiles(true);
            journalCleanup();

            verify(!haveJournalFiles()); // Double check post-conditions
        }
        
        class DurSSS : public ServerStatusSection {
        public:
            DurSSS() : ServerStatusSection( "dur" ){}
            virtual bool includeByDefault() const { return true; }
            
            BSONObj generateSection(const BSONElement& configElement) const {
                if (!storageGlobalParams.dur)
                    return BSONObj();
                return dur::stats.asObj();
            }
                
        } durSSS;


    } // namespace dur

} // namespace mongo
