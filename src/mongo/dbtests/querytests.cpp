// querytests.cpp : query.{h,cpp} unit tests.

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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_environment_d.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/global_optime.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/timer.h"

namespace mongo {
    void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip,
                         const BSONObj *fieldsToReturn, int queryOptions, Message &toSend );
}

namespace QueryTests {

    class Base {
    protected:
        OperationContextImpl _txn;
        Lock::GlobalWrite _lk;

        Client::Context _context;

        Database* _database;
        Collection* _collection;

    public:
        Base() : _lk(_txn.lockState()),
                 _context(ns()) {

            _database = _context.db();
            _collection = _database->getCollection( &_txn, ns() );
            if ( _collection ) {
                _database->dropCollection( &_txn, ns() );
            }
            _collection = _database->createCollection( &_txn, ns() );
            addIndex( fromjson( "{\"a\":1}" ) );
        }
        ~Base() {
            try {
                uassertStatusOK( _database->dropCollection( &_txn, ns() ) );
            }
            catch ( ... ) {
                FAIL( "Exception while cleaning up collection" );
            }
        }
    protected:
        static const char *ns() {
            return "unittests.querytests";
        }
        void addIndex( const BSONObj &key ) {
            BSONObjBuilder b;
            b.append( "name", key.firstElementFieldName() );
            b.append( "ns", ns() );
            b.append( "key", key );
            BSONObj o = b.done();
            Status s = _collection->getIndexCatalog()->createIndex(&_txn, o, false);
            uassertStatusOK( s );
        }
        void insert( const char *s ) {
            insert( fromjson( s ) );
        }
        void insert( const BSONObj &o ) {
            if ( o["_id"].eoo() ) {
                BSONObjBuilder b;
                OID oid;
                oid.init();
                b.appendOID( "_id", &oid );
                b.appendElements( o );
                _collection->insertDocument( &_txn, b.obj(), false );
            }
            else {
                _collection->insertDocument( &_txn, o, false );
            }
        }
    };

    class FindOneOr : public Base {
    public:
        void run() {
            addIndex( BSON( "b" << 1 ) );
            addIndex( BSON( "c" << 1 ) );
            insert( BSON( "b" << 2 << "_id" << 0 ) );
            insert( BSON( "c" << 3 << "_id" << 1 ) );
            BSONObj query = fromjson( "{$or:[{b:2},{c:3}]}" );
            BSONObj ret;
            // Check findOne() returning object.
            ASSERT( Helpers::findOne( &_txn, _collection, query, ret, true ) );
            ASSERT_EQUALS( string( "b" ), ret.firstElement().fieldName() );
            // Cross check with findOne() returning location.
            ASSERT_EQUALS(ret, _collection->docFor(Helpers::findOne(&_txn, _collection, query, true)));
        }
    };
    
    class FindOneRequireIndex : public Base {
    public:
        void run() {
            insert( BSON( "b" << 2 << "_id" << 0 ) );
            BSONObj query = fromjson( "{b:2}" );
            BSONObj ret;

            // Check findOne() returning object, allowing unindexed scan.
            ASSERT( Helpers::findOne( &_txn, _collection, query, ret, false ) );
            // Check findOne() returning location, allowing unindexed scan.
            ASSERT_EQUALS(ret, _collection->docFor(Helpers::findOne(&_txn, _collection, query, false)));
            
            // Check findOne() returning object, requiring indexed scan without index.
            ASSERT_THROWS( Helpers::findOne( &_txn, _collection, query, ret, true ), MsgAssertionException );
            // Check findOne() returning location, requiring indexed scan without index.
            ASSERT_THROWS( Helpers::findOne( &_txn, _collection, query, true ), MsgAssertionException );

            addIndex( BSON( "b" << 1 ) );
            // Check findOne() returning object, requiring indexed scan with index.
            ASSERT( Helpers::findOne( &_txn, _collection, query, ret, true ) );
            // Check findOne() returning location, requiring indexed scan with index.
            ASSERT_EQUALS(ret, _collection->docFor(Helpers::findOne(&_txn, _collection, query, true)));
        }
    };
    
    class FindOneEmptyObj : public Base {
    public:
        void run() {
            // We don't normally allow empty objects in the database, but test that we can find
            // an empty object (one might be allowed inside a reserved namespace at some point).
            Lock::GlobalWrite lk(_txn.lockState());
            Client::Context ctx( "unittests.querytests" );

            Database* db = ctx.db();
            if ( db->getCollection( &_txn, ns() ) ) {
                _collection = NULL;
                db->dropCollection( &_txn, ns() );
            }
            _collection = db->createCollection( &_txn, ns(), CollectionOptions(), true, false );
            ASSERT( _collection );

            DBDirectClient cl;
            BSONObj info;
            bool ok = cl.runCommand( "unittests", BSON( "godinsert" << "querytests" << "obj" << BSONObj() ), info );
            ASSERT( ok );

            insert( BSONObj() );
            BSONObj query;
            BSONObj ret;
            ASSERT( Helpers::findOne( &_txn, _collection, query, ret, false ) );
            ASSERT( ret.isEmpty() );
            ASSERT_EQUALS(ret, _collection->docFor(Helpers::findOne(&_txn, _collection, query, false)));
        }
    };
    
    class ClientBase {
    public:
        ClientBase() {
            mongo::lastError.reset( new LastError() );
        }
        ~ClientBase() {

        }

    protected:
        void insert( const char *ns, BSONObj o ) {
            client_.insert( ns, o );
        }
        void update( const char *ns, BSONObj q, BSONObj o, bool upsert = 0 ) {
            client_.update( ns, Query( q ), o, upsert );
        }
        bool error() {
            return !client_.getPrevError().getField( "err" ).isNull();
        }

        const DBDirectClient& client() const { return client_; }
        DBDirectClient& client() { return client_; }

        DBDirectClient client_;

        OperationContextImpl _txn;
    };

    class BoundedKey : public ClientBase {
    public:
        ~BoundedKey() {
            client().dropCollection( "unittests.querytests.BoundedKey" );
        }
        void run() {
            const char *ns = "unittests.querytests.BoundedKey";
            insert( ns, BSON( "a" << 1 ) );
            BSONObjBuilder a;
            a.appendMaxKey( "$lt" );
            BSONObj limit = a.done();
            ASSERT( !client().findOne( ns, QUERY( "a" << limit ) ).isEmpty() );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( !client().findOne( ns, QUERY( "a" << limit ).hint( BSON( "a" << 1 ) ) ).isEmpty() );
        }
    };

    class GetMore : public ClientBase {
    public:
        ~GetMore() {
            client().dropCollection( "unittests.querytests.GetMore" );
        }
        void run() {
            const char *ns = "unittests.querytests.GetMore";
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            insert( ns, BSON( "a" << 3 ) );
            auto_ptr< DBClientCursor > cursor = client().query( ns, BSONObj(), 2 );
            long long cursorId = cursor->getCursorId();
            cursor->decouple();
            cursor.reset();

            {
                // Check internal server handoff to getmore.
                Lock::DBWrite lk(_txn.lockState(), ns);
                Client::Context ctx( ns );
                ClientCursorPin clientCursor( ctx.db()->getCollection(&_txn, ns), cursorId );
                // pq doesn't exist if it's a runner inside of the clientcursor.
                // ASSERT( clientCursor.c()->pq );
                // ASSERT_EQUALS( 2, clientCursor.c()->pq->getNumToReturn() );
                ASSERT_EQUALS( 2, clientCursor.c()->pos() );
            }
            
            cursor = client().getMore( ns, cursorId );
            ASSERT( cursor->more() );
            ASSERT_EQUALS( 3, cursor->next().getIntField( "a" ) );
        }

    protected:
        OperationContextImpl _txn;
    };

    /**
     * An exception triggered during a get more request destroys the ClientCursor used by the get
     * more, preventing further iteration of the cursor in subsequent get mores.
     */
    class GetMoreKillOp : public ClientBase {
    public:
        ~GetMoreKillOp() {
            getGlobalEnvironment()->unsetKillAllOperations();
            client().dropCollection( "unittests.querytests.GetMoreKillOp" );
        }
        void run() {
            
            // Create a collection with some data.
            const char* ns = "unittests.querytests.GetMoreKillOp";
            for( int i = 0; i < 1000; ++i ) {
                insert( ns, BSON( "a" << i ) );
            }

            // Create a cursor on the collection, with a batch size of 200.
            auto_ptr<DBClientCursor> cursor = client().query( ns, "", 0, 0, 0, 0, 200 );
            CursorId cursorId = cursor->getCursorId();
            
            // Count 500 results, spanning a few batches of documents.
            for( int i = 0; i < 500; ++i ) {
                ASSERT( cursor->more() );
                cursor->next();
            }
            
            // Set the killop kill all flag, forcing the next get more to fail with a kill op
            // exception.
            getGlobalEnvironment()->setKillAllOperations();
            while( cursor->more() ) {
                cursor->next();
            }
            
            // Revert the killop kill all flag.
            getGlobalEnvironment()->unsetKillAllOperations();

            // Check that the cursor has been removed.
            {
                Client::ReadContext ctx(&_txn, ns);
                ASSERT(0 == ctx.ctx().db()->getCollection(&_txn, ns)->cursorCache()->numCursors());
            }

            ASSERT_FALSE(CollectionCursorCache::eraseCursorGlobal(&_txn, cursorId));

            // Check that a subsequent get more fails with the cursor removed.
            ASSERT_THROWS( client().getMore( ns, cursorId ), UserException );
        }
    };

    /**
     * A get more exception caused by an invalid or unauthorized get more request does not cause
     * the get more's ClientCursor to be destroyed.  This prevents an unauthorized user from
     * improperly killing a cursor by issuing an invalid get more request.
     */
    class GetMoreInvalidRequest : public ClientBase {
    public:
        ~GetMoreInvalidRequest() {
            getGlobalEnvironment()->unsetKillAllOperations();
            client().dropCollection( "unittests.querytests.GetMoreInvalidRequest" );
        }
        void run() {

            // Create a collection with some data.
            const char* ns = "unittests.querytests.GetMoreInvalidRequest";
            for( int i = 0; i < 1000; ++i ) {
                insert( ns, BSON( "a" << i ) );
            }
            
            // Create a cursor on the collection, with a batch size of 200.
            auto_ptr<DBClientCursor> cursor = client().query( ns, "", 0, 0, 0, 0, 200 );
            CursorId cursorId = cursor->getCursorId();

            // Count 500 results, spanning a few batches of documents.
            int count = 0;
            for( int i = 0; i < 500; ++i ) {
                ASSERT( cursor->more() );
                cursor->next();
                ++count;
            }

            // Send a get more with a namespace that is incorrect ('spoofed') for this cursor id.
            // This is the invalaid get more request described in the comment preceding this class.
            client().getMore
                    ( "unittests.querytests.GetMoreInvalidRequest_WRONG_NAMESPACE_FOR_CURSOR",
                      cursor->getCursorId() );

            // Check that the cursor still exists
            {
                Client::ReadContext ctx(&_txn, ns);
                ASSERT( 1 == ctx.ctx().db()->getCollection( &_txn, ns )->cursorCache()->numCursors() );
                ASSERT( ctx.ctx().db()->getCollection( &_txn, ns )->cursorCache()->find( cursorId, false ) );
            }

            // Check that the cursor can be iterated until all documents are returned.
            while( cursor->more() ) {
                cursor->next();
                ++count;
            }
            ASSERT_EQUALS( 1000, count );
        }
    };
    
    class PositiveLimit : public ClientBase {
    public:
        const char* ns;
        PositiveLimit() : ns("unittests.querytests.PositiveLimit") {}
        ~PositiveLimit() {
            client().dropCollection( ns );
        }

        void testLimit(int limit) {
            ASSERT_EQUALS(client().query( ns, BSONObj(), limit )->itcount(), limit);
        }
        void run() {
            for(int i=0; i<1000; i++)
                insert( ns, BSON( GENOID << "i" << i ) );

            ASSERT_EQUALS( client().query(ns, BSONObj(),    1 )->itcount(), 1);
            ASSERT_EQUALS( client().query(ns, BSONObj(),   10 )->itcount(), 10);
            ASSERT_EQUALS( client().query(ns, BSONObj(),  101 )->itcount(), 101);
            ASSERT_EQUALS( client().query(ns, BSONObj(),  999 )->itcount(), 999);
            ASSERT_EQUALS( client().query(ns, BSONObj(), 1000 )->itcount(), 1000);
            ASSERT_EQUALS( client().query(ns, BSONObj(), 1001 )->itcount(), 1000);
            ASSERT_EQUALS( client().query(ns, BSONObj(),    0 )->itcount(), 1000);
        }
    };

    class ReturnOneOfManyAndTail : public ClientBase {
    public:
        ~ReturnOneOfManyAndTail() {
            client().dropCollection( "unittests.querytests.ReturnOneOfManyAndTail" );
        }
        void run() {
            const char *ns = "unittests.querytests.ReturnOneOfManyAndTail";
            client().createCollection( ns, 1024, true );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "a" << GT << 0 ).hint( BSON( "$natural" << 1 ) ), 1, 0, 0, QueryOption_CursorTailable );
            // If only one result requested, a cursor is not saved.
            ASSERT_EQUALS( 0, c->getCursorId() );
            ASSERT( c->more() );
            ASSERT_EQUALS( 1, c->next().getIntField( "a" ) );
        }
    };

    class TailNotAtEnd : public ClientBase {
    public:
        ~TailNotAtEnd() {
            client().dropCollection( "unittests.querytests.TailNotAtEnd" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailNotAtEnd";
            client().createCollection( ns, 2047, true );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            insert( ns, BSON( "a" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            ASSERT( 0 != c->getCursorId() );
            while( c->more() )
                c->next();
            ASSERT( 0 != c->getCursorId() );
            insert( ns, BSON( "a" << 3 ) );
            insert( ns, BSON( "a" << 4 ) );
            insert( ns, BSON( "a" << 5 ) );
            insert( ns, BSON( "a" << 6 ) );
            ASSERT( c->more() );
            ASSERT_EQUALS( 3, c->next().getIntField( "a" ) );
        }
    };

    class EmptyTail : public ClientBase {
    public:
        ~EmptyTail() {
            client().dropCollection( "unittests.querytests.EmptyTail" );
        }
        void run() {
            const char *ns = "unittests.querytests.EmptyTail";
            client().createCollection( ns, 1900, true );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            ASSERT_EQUALS( 0, c->getCursorId() );
            ASSERT( c->isDead() );
            insert( ns, BSON( "a" << 0 ) );
            c = client().query( ns, QUERY( "a" << 1 ).hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            ASSERT( 0 != c->getCursorId() );
            ASSERT( !c->isDead() );
        }
    };

    class TailableDelete : public ClientBase {
    public:
        ~TailableDelete() {
            client().dropCollection( "unittests.querytests.TailableDelete" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailableDelete";
            client().createCollection( ns, 8192, true, 2 );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            c->next();
            c->next();
            ASSERT( !c->more() );
            insert( ns, BSON( "a" << 2 ) );
            insert( ns, BSON( "a" << 3 ) );
            ASSERT( !c->more() );
            // Inserting a document into a capped collection can force another document out.
            // In this case, the capped collection has 2 documents, so inserting two more clobbers
            // whatever DiskLoc that the underlying cursor had as its state.
            //
            // In the Cursor world, the ClientCursor was responsible for manipulating cursors.  It
            // would detect that the cursor's "refloc" (translation: diskloc required to maintain
            // iteration state) was being clobbered and it would kill the cursor.
            //
            // In the Runner world there is no notion of a "refloc" and as such the invalidation
            // broadcast code doesn't know enough to know that the underlying collection iteration
            // can't proceed.
            // ASSERT_EQUALS( 0, c->getCursorId() );
        }
    };

    class TailableInsertDelete : public ClientBase {
    public:
        ~TailableInsertDelete() {
            client().dropCollection( "unittests.querytests.TailableInsertDelete" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailableInsertDelete";
            client().createCollection( ns, 1330, true );
            insert( ns, BSON( "a" << 0 ) );
            insert( ns, BSON( "a" << 1 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, Query().hint( BSON( "$natural" << 1 ) ), 2, 0, 0, QueryOption_CursorTailable );
            c->next();
            c->next();
            ASSERT( !c->more() );
            insert( ns, BSON( "a" << 2 ) );
            client().remove( ns, QUERY( "a" << 1 ) );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "a" ) );
            ASSERT( !c->more() );
        }
    };

    class TailCappedOnly : public ClientBase {
    public:
        ~TailCappedOnly() {
            client().dropCollection( "unittest.querytests.TailCappedOnly" );
        }
        void run() {
            const char *ns = "unittests.querytests.TailCappedOnly";
            client().insert( ns, BSONObj() );
            auto_ptr< DBClientCursor > c = client().query( ns, BSONObj(), 0, 0, 0, QueryOption_CursorTailable );
            ASSERT( c->isDead() );
            ASSERT( !client().getLastError().empty() );
        }
    };

    class TailableQueryOnId : public ClientBase {
    public:
        ~TailableQueryOnId() {
            client().dropCollection( "unittests.querytests.TailableQueryOnId" );
        }

		void insertA(const char* ns, int a) {
			BSONObjBuilder b;
			b.appendOID("_id", 0, true);
			b.appendOID("value", 0, true);
			b.append("a", a);
			insert(ns, b.obj());
		}

        void run() {
            const char *ns = "unittests.querytests.TailableQueryOnId";
            BSONObj info;
            client().runCommand( "unittests", BSON( "create" << "querytests.TailableQueryOnId" << "capped" << true << "size" << 8192 << "autoIndexId" << true ), info );
            insertA( ns, 0 );
            insertA( ns, 1 );
            auto_ptr< DBClientCursor > c1 = client().query( ns, QUERY( "a" << GT << -1 ), 0, 0, 0, QueryOption_CursorTailable );
            OID id;
            id.init("000000000000000000000000");
            auto_ptr< DBClientCursor > c2 = client().query( ns, QUERY( "value" << GT << id ), 0, 0, 0, QueryOption_CursorTailable );
            c1->next();
            c1->next();
            ASSERT( !c1->more() );
            c2->next();
            c2->next();
            ASSERT( !c2->more() );
            insertA( ns, 2 );
            ASSERT( c1->more() );
            ASSERT_EQUALS( 2, c1->next().getIntField( "a" ) );
            ASSERT( !c1->more() );
            ASSERT( c2->more() );
            ASSERT_EQUALS( 2, c2->next().getIntField( "a" ) );  // SERVER-645
            ASSERT( !c2->more() );
            ASSERT( !c2->isDead() );
        }
    };

    class OplogReplayMode : public ClientBase {
    public:
        ~OplogReplayMode() {
            client().dropCollection( "unittests.querytests.OplogReplayMode" );
        }
        void run() {
            const char *ns = "unittests.querytests.OplogReplayMode";
            insert( ns, BSON( "ts" << 0 ) );
            insert( ns, BSON( "ts" << 1 ) );
            insert( ns, BSON( "ts" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query( ns, QUERY( "ts" << GT << 1 ).hint( BSON( "$natural" << 1 ) ), 0, 0, 0, QueryOption_OplogReplay );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "ts" ) );
            ASSERT( !c->more() );

            insert( ns, BSON( "ts" << 3 ) );
            c = client().query( ns, QUERY( "ts" << GT << 1 ).hint( BSON( "$natural" << 1 ) ), 0, 0, 0, QueryOption_OplogReplay );
            ASSERT( c->more() );
            ASSERT_EQUALS( 2, c->next().getIntField( "ts" ) );
            ASSERT( c->more() );
        }
    };

    class OplogReplaySlaveReadTill : public ClientBase {
    public:
        ~OplogReplaySlaveReadTill() {
            client().dropCollection( "unittests.querytests.OplogReplaySlaveReadTill" );
        }
        void run() {
            const char *ns = "unittests.querytests.OplogReplaySlaveReadTill";
            Lock::DBWrite lk(_txn.lockState(), ns);
            Client::Context ctx( ns );
            
            BSONObj info;
            client().runCommand( "unittests",
                                BSON( "create" << "querytests.OplogReplaySlaveReadTill" <<
                                     "capped" << true << "size" << 8192 ),
                                info );

            Date_t one = getNextGlobalOptime().asDate();
            Date_t two = getNextGlobalOptime().asDate();
            Date_t three = getNextGlobalOptime().asDate();
            insert( ns, BSON( "ts" << one ) );
            insert( ns, BSON( "ts" << two ) );
            insert( ns, BSON( "ts" << three ) );
            auto_ptr<DBClientCursor> c =
            client().query( ns, QUERY( "ts" << GTE << two ).hint( BSON( "$natural" << 1 ) ),
                           0, 0, 0, QueryOption_OplogReplay | QueryOption_CursorTailable );
            ASSERT( c->more() );
            ASSERT_EQUALS( two, c->next()["ts"].Date() );
            long long cursorId = c->getCursorId();
            
            ClientCursorPin clientCursor( ctx.db()->getCollection( &_txn, ns ), cursorId );
            ASSERT_EQUALS( three.millis, clientCursor.c()->getSlaveReadTill().asDate() );
        }
    };

    class OplogReplayExplain : public ClientBase {
    public:
        ~OplogReplayExplain() {
            client().dropCollection( "unittests.querytests.OplogReplayExplain" );
        }
        void run() {
            const char *ns = "unittests.querytests.OplogReplayExplain";
            insert( ns, BSON( "ts" << 0 ) );
            insert( ns, BSON( "ts" << 1 ) );
            insert( ns, BSON( "ts" << 2 ) );
            auto_ptr< DBClientCursor > c = client().query(
                ns, QUERY( "ts" << GT << 1 ).hint( BSON( "$natural" << 1 ) ).explain(),
                0, 0, 0, QueryOption_OplogReplay );
            ASSERT( c->more() );

            // Check number of results and filterSet flag in explain.
            // filterSet is not available in oplog replay mode.
            BSONObj explainObj = c->next();
            ASSERT_EQUALS( 1, explainObj.getIntField( "n" ) );
            ASSERT_FALSE( explainObj.hasField( "filterSet" ) );

            ASSERT( !c->more() );
        }
    };

    class BasicCount : public ClientBase {
    public:
        ~BasicCount() {
            client().dropCollection( "unittests.querytests.BasicCount" );
        }
        void run() {
            const char *ns = "unittests.querytests.BasicCount";
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            count( 0 );
            insert( ns, BSON( "a" << 3 ) );
            count( 0 );
            insert( ns, BSON( "a" << 4 ) );
            count( 1 );
            insert( ns, BSON( "a" << 5 ) );
            count( 1 );
            insert( ns, BSON( "a" << 4 ) );
            count( 2 );
        }
    private:
        void count( unsigned long long c ) {
            ASSERT_EQUALS( c, client().count( "unittests.querytests.BasicCount", BSON( "a" << 4 ) ) );
        }
    };

    class ArrayId : public ClientBase {
    public:
        ~ArrayId() {
            client().dropCollection( "unittests.querytests.ArrayId" );
        }
        void run() {
            const char *ns = "unittests.querytests.ArrayId";
            client().ensureIndex( ns, BSON( "_id" << 1 ) );
            ASSERT( !error() );
            client().insert( ns, fromjson( "{'_id':[1,2]}" ) );
            ASSERT( error() );
        }
    };

    class UnderscoreNs : public ClientBase {
    public:
        ~UnderscoreNs() {
            client().dropCollection( "unittests.querytests._UnderscoreNs" );
        }
        void run() {
            ASSERT( !error() );
            const char *ns = "unittests.querytests._UnderscoreNs";
            ASSERT( client().findOne( ns, "{}" ).isEmpty() );
            client().insert( ns, BSON( "a" << 1 ) );
            ASSERT_EQUALS( 1, client().findOne( ns, "{}" ).getIntField( "a" ) );
            ASSERT( !error() );
        }
    };

    class EmptyFieldSpec : public ClientBase {
    public:
        ~EmptyFieldSpec() {
            client().dropCollection( "unittests.querytests.EmptyFieldSpec" );
        }
        void run() {
            const char *ns = "unittests.querytests.EmptyFieldSpec";
            client().insert( ns, BSON( "a" << 1 ) );
            ASSERT( !client().findOne( ns, "" ).isEmpty() );
            BSONObj empty;
            ASSERT( !client().findOne( ns, "", &empty ).isEmpty() );
        }
    };

    class MultiNe : public ClientBase {
    public:
        ~MultiNe() {
            client().dropCollection( "unittests.querytests.Ne" );
        }
        void run() {
            const char *ns = "unittests.querytests.Ne";
            client().insert( ns, fromjson( "{a:[1,2]}" ) );
            ASSERT( client().findOne( ns, fromjson( "{a:{$ne:1}}" ) ).isEmpty() );
            BSONObj spec = fromjson( "{a:{$ne:1,$ne:2}}" );
            ASSERT( client().findOne( ns, spec ).isEmpty() );
        }
    };

    class EmbeddedNe : public ClientBase {
    public:
        ~EmbeddedNe() {
            client().dropCollection( "unittests.querytests.NestedNe" );
        }
        void run() {
            const char *ns = "unittests.querytests.NestedNe";
            client().insert( ns, fromjson( "{a:[{b:1},{b:2}]}" ) );
            ASSERT( client().findOne( ns, fromjson( "{'a.b':{$ne:1}}" ) ).isEmpty() );
        }
    };

    class EmbeddedNumericTypes : public ClientBase {
    public:
        ~EmbeddedNumericTypes() {
            client().dropCollection( "unittests.querytests.NumericEmbedded" );
        }
        void run() {
            const char *ns = "unittests.querytests.NumericEmbedded";
            client().insert( ns, BSON( "a" << BSON ( "b" << 1 ) ) );
            ASSERT( ! client().findOne( ns, BSON( "a" << BSON ( "b" << 1.0 ) ) ).isEmpty() );
            client().ensureIndex( ns , BSON( "a" << 1 ) );
            ASSERT( ! client().findOne( ns, BSON( "a" << BSON ( "b" << 1.0 ) ) ).isEmpty() );
        }
    };

    class AutoResetIndexCache : public ClientBase {
    public:
        ~AutoResetIndexCache() {
            client().dropCollection( "unittests.querytests.AutoResetIndexCache" );
        }
        static const char *ns() { return "unittests.querytests.AutoResetIndexCache"; }
        static const char *idxNs() { return "unittests.system.indexes"; }
        void index() { ASSERT( !client().findOne( idxNs(), BSON( "name" << NE << "_id_" ) ).isEmpty() ); }
        void noIndex() {
            BSONObj o = client().findOne( idxNs(), BSON( "name" << NE << "_id_" ) );
            if( !o.isEmpty() ) {
                cout << o.toString() << endl;
                ASSERT( false );
            }
        }
        void checkIndex() {
            client().ensureIndex( ns(), BSON( "a" << 1 ) );
            index();
        }
        void run() {
            client().dropDatabase( "unittests" );
            noIndex();
            checkIndex();
            client().dropCollection( ns() );
            noIndex();
            checkIndex();
            client().dropDatabase( "unittests" );
            noIndex();
            checkIndex();
        }
    };

    class UniqueIndex : public ClientBase {
    public:
        ~UniqueIndex() {
            client().dropCollection( "unittests.querytests.UniqueIndex" );
        }
        void run() {
            const char *ns = "unittests.querytests.UniqueIndex";
            client().ensureIndex( ns, BSON( "a" << 1 ), true );
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            ASSERT_EQUALS( 1U, client().count( ns, BSONObj() ) );
            client().dropCollection( ns );
            client().ensureIndex( ns, BSON( "b" << 1 ), true );
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            ASSERT_EQUALS( 2U, client().count( ns, BSONObj() ) );
        }
    };

    class UniqueIndexPreexistingData : public ClientBase {
    public:
        ~UniqueIndexPreexistingData() {
            client().dropCollection( "unittests.querytests.UniqueIndexPreexistingData" );
        }
        void run() {
            const char *ns = "unittests.querytests.UniqueIndexPreexistingData";
            client().insert( ns, BSON( "a" << 4 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 4 << "b" << 3 ) );
            client().ensureIndex( ns, BSON( "a" << 1 ), true );
            ASSERT_EQUALS( 0U, client().count( "unittests.system.indexes", BSON( "ns" << ns << "name" << NE << "_id_" ) ) );
        }
    };

    class SubobjectInArray : public ClientBase {
    public:
        ~SubobjectInArray() {
            client().dropCollection( "unittests.querytests.SubobjectInArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.SubobjectInArray";
            client().insert( ns, fromjson( "{a:[{b:{c:1}}]}" ) );
            ASSERT( !client().findOne( ns, BSON( "a.b.c" << 1 ) ).isEmpty() );
            ASSERT( !client().findOne( ns, fromjson( "{'a.c':null}" ) ).isEmpty() );
        }
    };

    class Size : public ClientBase {
    public:
        ~Size() {
            client().dropCollection( "unittests.querytests.Size" );
        }
        void run() {
            const char *ns = "unittests.querytests.Size";
            client().insert( ns, fromjson( "{a:[1,2,3]}" ) );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( client().query( ns, QUERY( "a" << mongo::BSIZE << 3 ).hint( BSON( "a" << 1 ) ) )->more() );
        }
    };

    class FullArray : public ClientBase {
    public:
        ~FullArray() {
            client().dropCollection( "unittests.querytests.IndexedArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.IndexedArray";
            client().insert( ns, fromjson( "{a:[1,2,3]}" ) );
            ASSERT( client().query( ns, Query( "{a:[1,2,3]}" ) )->more() );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT( client().query( ns, Query( "{a:{$in:[1,[1,2,3]]}}" ).hint( BSON( "a" << 1 ) ) )->more() );
            ASSERT( client().query( ns, Query( "{a:[1,2,3]}" ).hint( BSON( "a" << 1 ) ) )->more() ); // SERVER-146
        }
    };

    class InsideArray : public ClientBase {
    public:
        ~InsideArray() {
            client().dropCollection( "unittests.querytests.InsideArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.InsideArray";
            client().insert( ns, fromjson( "{a:[[1],2]}" ) );
            check( "$natural" );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            check( "a" ); // SERVER-146
        }
    private:
        void check( const string &hintField ) {
            const char *ns = "unittests.querytests.InsideArray";
            ASSERT( client().query( ns, Query( "{a:[[1],2]}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( client().query( ns, Query( "{a:[1]}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( client().query( ns, Query( "{a:2}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( !client().query( ns, Query( "{a:1}" ).hint( BSON( hintField << 1 ) ) )->more() );
        }
    };

    class IndexInsideArrayCorrect : public ClientBase {
    public:
        ~IndexInsideArrayCorrect() {
            client().dropCollection( "unittests.querytests.IndexInsideArrayCorrect" );
        }
        void run() {
            const char *ns = "unittests.querytests.IndexInsideArrayCorrect";
            client().insert( ns, fromjson( "{'_id':1,a:[1]}" ) );
            client().insert( ns, fromjson( "{'_id':2,a:[[1]]}" ) );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            ASSERT_EQUALS( 1, client().query( ns, Query( "{a:[1]}" ).hint( BSON( "a" << 1 ) ) )->next().getIntField( "_id" ) );
        }
    };

    class SubobjArr : public ClientBase {
    public:
        ~SubobjArr() {
            client().dropCollection( "unittests.querytests.SubobjArr" );
        }
        void run() {
            const char *ns = "unittests.querytests.SubobjArr";
            client().insert( ns, fromjson( "{a:[{b:[1]}]}" ) );
            check( "$natural" );
            client().ensureIndex( ns, BSON( "a" << 1 ) );
            check( "a" );
        }
    private:
        void check( const string &hintField ) {
            const char *ns = "unittests.querytests.SubobjArr";
            ASSERT( client().query( ns, Query( "{'a.b':1}" ).hint( BSON( hintField << 1 ) ) )->more() );
            ASSERT( client().query( ns, Query( "{'a.b':[1]}" ).hint( BSON( hintField << 1 ) ) )->more() );
        }
    };

    class MinMax : public ClientBase {
    public:
        MinMax() : ns( "unittests.querytests.MinMax" ) {}
        ~MinMax() {
            client().dropCollection( "unittests.querytests.MinMax" );
        }
        void run() {
            client().ensureIndex( ns, BSON( "a" << 1 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 1 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 1 << "b" << 2 ) );
            client().insert( ns, BSON( "a" << 2 << "b" << 1 ) );
            client().insert( ns, BSON( "a" << 2 << "b" << 2 ) );

            ASSERT_EQUALS( 4, count( client().query( ns, BSONObj() ) ) );
            BSONObj hints[] = { BSONObj(), BSON( "a" << 1 << "b" << 1 ) };
            for( int i = 0; i < 2; ++i ) {
                check( 0, 0, 3, 3, 4, hints[ i ] );
                check( 1, 1, 2, 2, 3, hints[ i ] );
                check( 1, 2, 2, 2, 2, hints[ i ] );
                check( 1, 2, 2, 1, 1, hints[ i ] );

                auto_ptr< DBClientCursor > c = query( 1, 2, 2, 2, hints[ i ] );
                BSONObj obj = c->next();
                ASSERT_EQUALS( 1, obj.getIntField( "a" ) );
                ASSERT_EQUALS( 2, obj.getIntField( "b" ) );
                obj = c->next();
                ASSERT_EQUALS( 2, obj.getIntField( "a" ) );
                ASSERT_EQUALS( 1, obj.getIntField( "b" ) );
                ASSERT( !c->more() );
            }
        }
    private:
        auto_ptr< DBClientCursor > query( int minA, int minB, int maxA, int maxB, const BSONObj &hint ) {
            Query q;
            q = q.minKey( BSON( "a" << minA << "b" << minB ) ).maxKey( BSON( "a" << maxA << "b" << maxB ) );
            if ( !hint.isEmpty() )
                q.hint( hint );
            return client().query( ns, q );
        }
        void check( int minA, int minB, int maxA, int maxB, int expectedCount, const BSONObj &hint = empty_ ) {
            ASSERT_EQUALS( expectedCount, count( query( minA, minB, maxA, maxB, hint ) ) );
        }
        int count( auto_ptr< DBClientCursor > c ) {
            int ret = 0;
            while( c->more() ) {
                ++ret;
                c->next();
            }
            return ret;
        }
        const char *ns;
        static BSONObj empty_;
    };
    BSONObj MinMax::empty_;

    class MatchCodeCodeWScope : public ClientBase {
    public:
        MatchCodeCodeWScope() : _ns( "unittests.querytests.MatchCodeCodeWScope" ) {}
        ~MatchCodeCodeWScope() {
            client().dropCollection( "unittests.querytests.MatchCodeCodeWScope" );
        }
        void run() {
            checkMatch();
            client().ensureIndex( _ns, BSON( "a" << 1 ) );
            checkMatch();
            // Use explain queries to check index bounds.
            {
                BSONObj explain = client().findOne( _ns, QUERY( "a" << BSON( "$type" << (int)Code ) ).explain() );
                BSONObjBuilder lower;
                lower.appendCode( "", "" );
                BSONObjBuilder upper;
                upper.appendCodeWScope( "", "", BSONObj() );
                ASSERT( lower.done().firstElement().valuesEqual( explain[ "indexBounds" ].Obj()[ "a" ].Array()[ 0 ].Array()[ 0 ] ) );
                ASSERT( upper.done().firstElement().valuesEqual( explain[ "indexBounds" ].Obj()[ "a" ].Array()[ 0 ].Array()[ 1 ] ) );
            }
            {
                BSONObj explain = client().findOne( _ns, QUERY( "a" << BSON( "$type" << (int)CodeWScope ) ).explain() );
                BSONObjBuilder lower;
                lower.appendCodeWScope( "", "", BSONObj() );
                // This upper bound may change if a new bson type is added.
                BSONObjBuilder upper;
                upper << "" << BSON( "$maxElement" << 1 );
                ASSERT( lower.done().firstElement().valuesEqual( explain[ "indexBounds" ].Obj()[ "a" ].Array()[ 0 ].Array()[ 0 ] ) );
                ASSERT( upper.done().firstElement().valuesEqual( explain[ "indexBounds" ].Obj()[ "a" ].Array()[ 0 ].Array()[ 1 ] ) );
            }
        }
    private:
        void checkMatch() {
            client().remove( _ns, BSONObj() );
            
            client().insert( _ns, code() );
            client().insert( _ns, codeWScope() );
            
            ASSERT_EQUALS( 1U, client().count( _ns, code() ) );
            ASSERT_EQUALS( 1U, client().count( _ns, codeWScope() ) );
            
            ASSERT_EQUALS( 1U, client().count( _ns, BSON( "a" << BSON( "$type" << (int)Code ) ) ) );
            ASSERT_EQUALS( 1U, client().count( _ns, BSON( "a" << BSON( "$type" << (int)CodeWScope ) ) ) );
        }
        BSONObj code() const {
            BSONObjBuilder codeBuilder;
            codeBuilder.appendCode( "a", "return 1;" );
            return codeBuilder.obj();            
        }
        BSONObj codeWScope() const {
            BSONObjBuilder codeWScopeBuilder;
            codeWScopeBuilder.appendCodeWScope( "a", "return 1;", BSONObj() );
            return codeWScopeBuilder.obj();            
        }
        const char *_ns;
    };
    
    class MatchDBRefType : public ClientBase {
    public:
        MatchDBRefType() : _ns( "unittests.querytests.MatchDBRefType" ) {}
        ~MatchDBRefType() {
            client().dropCollection( "unittests.querytests.MatchDBRefType" );
        }
        void run() {
            checkMatch();
            client().ensureIndex( _ns, BSON( "a" << 1 ) );
            checkMatch();
        }
    private:
        void checkMatch() {
            client().remove( _ns, BSONObj() );            
            client().insert( _ns, dbref() );
            ASSERT_EQUALS( 1U, client().count( _ns, dbref() ) );
            ASSERT_EQUALS( 1U, client().count( _ns, BSON( "a" << BSON( "$type" << (int)DBRef ) ) ) );
        }
        BSONObj dbref() const {
            BSONObjBuilder b;
            OID oid;
            b.appendDBRef( "a", "ns", oid );
            return b.obj();            
        }
        const char *_ns;
    };
    
    class DirectLocking : public ClientBase {
    public:
        void run() {
            Lock::GlobalWrite lk(_txn.lockState());
            Client::Context ctx( "unittests.DirectLocking" );
            client().remove( "a.b", BSONObj() );
            ASSERT_EQUALS( "unittests", ctx.db()->name() );
        }
        const char *ns;
    };

    class FastCountIn : public ClientBase {
    public:
        ~FastCountIn() {
            client().dropCollection( "unittests.querytests.FastCountIn" );
        }
        void run() {
            const char *ns = "unittests.querytests.FastCountIn";
            client().insert( ns, BSON( "i" << "a" ) );
            client().ensureIndex( ns, BSON( "i" << 1 ) );
            ASSERT_EQUALS( 1U, client().count( ns, fromjson( "{i:{$in:['a']}}" ) ) );
        }
    };

    class EmbeddedArray : public ClientBase {
    public:
        ~EmbeddedArray() {
            client().dropCollection( "unittests.querytests.EmbeddedArray" );
        }
        void run() {
            const char *ns = "unittests.querytests.EmbeddedArray";
            client().insert( ns, fromjson( "{foo:{bar:['spam']}}" ) );
            client().insert( ns, fromjson( "{foo:{bar:['spam','eggs']}}" ) );
            client().insert( ns, fromjson( "{bar:['spam']}" ) );
            client().insert( ns, fromjson( "{bar:['spam','eggs']}" ) );
            ASSERT_EQUALS( 2U, client().count( ns, BSON( "bar" << "spam" ) ) );
            ASSERT_EQUALS( 2U, client().count( ns, BSON( "foo.bar" << "spam" ) ) );
        }
    };

    class DifferentNumbers : public ClientBase {
    public:
        ~DifferentNumbers() {
            client().dropCollection( "unittests.querytests.DifferentNumbers" );
        }
        void t( const char * ns ) {
            auto_ptr< DBClientCursor > cursor = client().query( ns, Query().sort( "7" ) );
            while ( cursor->more() ) {
                BSONObj o = cursor->next();
                verify( o.valid() );
                //cout << " foo " << o << endl;
            }

        }
        void run() {
            const char *ns = "unittests.querytests.DifferentNumbers";
            { BSONObjBuilder b; b.append( "7" , (int)4 ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.append( "7" , (long long)2 ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.appendNull( "7" ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.append( "7" , "b" ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.appendNull( "8" ); client().insert( ns , b.obj() ); }
            { BSONObjBuilder b; b.append( "7" , (double)3.7 ); client().insert( ns , b.obj() ); }

            t(ns);
            client().ensureIndex( ns , BSON( "7" << 1 ) );
            t(ns);
        }
    };

    class CollectionBase : public ClientBase {
    public:

        CollectionBase( string leaf ) {
            _ns = "unittests.querytests.";
            _ns += leaf;
            client().dropCollection( ns() );
        }

        virtual ~CollectionBase() {
            client().dropCollection( ns() );
        }

        int count() {
            return (int) client().count( ns() );
        }

        size_t numCursorsOpen() {
            Client::ReadContext ctx(&_txn, _ns);
            Collection* collection = ctx.ctx().db()->getCollection( &_txn, _ns );
            if ( !collection )
                return 0;
            return collection->cursorCache()->numCursors();
        }

        const char * ns() {
            return _ns.c_str();
        }

    private:
        string _ns;
    };

    class SymbolStringSame : public CollectionBase {
    public:
        SymbolStringSame() : CollectionBase( "symbolstringsame" ) {}

        void run() {
            { BSONObjBuilder b; b.appendSymbol( "x" , "eliot" ); b.append( "z" , 17 ); client().insert( ns() , b.obj() ); }
            ASSERT_EQUALS( 17 , client().findOne( ns() , BSONObj() )["z"].number() );
            {
                BSONObjBuilder b;
                b.appendSymbol( "x" , "eliot" );
                ASSERT_EQUALS( 17 , client().findOne( ns() , b.obj() )["z"].number() );
            }
            ASSERT_EQUALS( 17 , client().findOne( ns() , BSON( "x" << "eliot" ) )["z"].number() );
            client().ensureIndex( ns() , BSON( "x" << 1 ) );
            ASSERT_EQUALS( 17 , client().findOne( ns() , BSON( "x" << "eliot" ) )["z"].number() );
        }
    };

    class TailableCappedRaceCondition : public CollectionBase {
    public:

        TailableCappedRaceCondition() : CollectionBase( "tailablecappedrace" ) {
            client().dropCollection( ns() );
            _n = 0;
        }
        void run() {
            string err;
            Client::WriteContext ctx(&_txn,  "unittests" );

            // note that extents are always at least 4KB now - so this will get rounded up a bit.
            ASSERT( userCreateNS( &_txn, ctx.ctx().db(), ns(),
                                  fromjson( "{ capped : true, size : 2000 }" ), false ).isOK() );
            for ( int i=0; i<200; i++ ) {
                insertNext();
                ASSERT( count() < 90 );
            }

            int a = count();

            auto_ptr< DBClientCursor > c = client().query( ns() , QUERY( "i" << GT << 0 ).hint( BSON( "$natural" << 1 ) ), 0, 0, 0, QueryOption_CursorTailable );
            int n=0;
            while ( c->more() ) {
                BSONObj z = c->next();
                n++;
            }

            ASSERT_EQUALS( a , n );

            insertNext();
            ASSERT( c->more() );

            for ( int i=0; i<90; i++ ) {
                insertNext();
            }

            while ( c->more() ) { c->next(); }
            ASSERT( c->isDead() );
        }

        void insertNext() {
			BSONObjBuilder b;
			b.appendOID("_id", 0, true);
			b.append("i", _n++);
            insert( ns() , b.obj() );
        }

        int _n;
    };

    class HelperTest : public CollectionBase {
    public:

        HelperTest() : CollectionBase( "helpertest" ) {
        }

        void run() {
            Client::WriteContext ctx(&_txn,  "unittests" );

            for ( int i=0; i<50; i++ ) {
                insert( ns() , BSON( "_id" << i << "x" << i * 2 ) );
            }

            ASSERT_EQUALS( 50 , count() );

            BSONObj res;
            ASSERT( Helpers::findOne( &_txn, ctx.ctx().db()->getCollection( &_txn, ns() ),
                                      BSON( "_id" << 20 ) , res , true ) );
            ASSERT_EQUALS( 40 , res["x"].numberInt() );

            ASSERT( Helpers::findById( &_txn, ctx.ctx().db(), ns() , BSON( "_id" << 20 ) , res ) );
            ASSERT_EQUALS( 40 , res["x"].numberInt() );

            ASSERT( ! Helpers::findById( &_txn, ctx.ctx().db(), ns() , BSON( "_id" << 200 ) , res ) );

            long long slow;
            long long fast;

            int n = 10000;
            DEV n = 1000;
            {
                Timer t;
                for ( int i=0; i<n; i++ ) {
                    ASSERT( Helpers::findOne( &_txn, ctx.ctx().db()->getCollection(&_txn, ns()),
                                              BSON( "_id" << 20 ), res, true ) );
                }
                slow = t.micros();
            }
            {
                Timer t;
                for ( int i=0; i<n; i++ ) {
                    ASSERT( Helpers::findById( &_txn, ctx.ctx().db(), ns() , BSON( "_id" << 20 ) , res ) );
                }
                fast = t.micros();
            }

            cout << "HelperTest  slow:" << slow << " fast:" << fast << endl;

        }
    };

    class HelperByIdTest : public CollectionBase {
    public:

        HelperByIdTest() : CollectionBase( "helpertestbyid" ) {
        }

        void run() {
            Client::WriteContext ctx(&_txn,  "unittests" );

            for ( int i=0; i<1000; i++ ) {
                insert( ns() , BSON( "_id" << i << "x" << i * 2 ) );
            }
            for ( int i=0; i<1000; i+=2 ) {
                client_.remove( ns() , BSON( "_id" << i ) );
            }

            BSONObj res;
            for ( int i=0; i<1000; i++ ) {
                bool found = Helpers::findById( &_txn, ctx.ctx().db(), ns() , BSON( "_id" << i ) , res );
                ASSERT_EQUALS( i % 2 , int(found) );
            }

        }
    };

    class ClientCursorTest : public CollectionBase {
        ClientCursorTest() : CollectionBase( "clientcursortest" ) {
        }

        void run() {
            Client::WriteContext ctx(&_txn,  "unittests" );

            for ( int i=0; i<1000; i++ ) {
                insert( ns() , BSON( "_id" << i << "x" << i * 2 ) );
            }


        }
    };

    class FindingStart : public CollectionBase {
    public:
        FindingStart() : CollectionBase( "findingstart" ) {
        }

        void run() {
            BSONObj info;
            ASSERT( client().runCommand( "unittests", BSON( "create" << "querytests.findingstart" << "capped" << true << "$nExtents" << 5 << "autoIndexId" << false ), info ) );

            int i = 0;
            for( int oldCount = -1;
                    count() != oldCount;
                    oldCount = count(), client().insert( ns(), BSON( "ts" << i++ ) ) );

            for( int k = 0; k < 5; ++k ) {
                client().insert( ns(), BSON( "ts" << i++ ) );
                int min = client().query( ns(), Query().sort( BSON( "$natural" << 1 ) ) )->next()[ "ts" ].numberInt();
                for( int j = -1; j < i; ++j ) {
                    auto_ptr< DBClientCursor > c = client().query( ns(), QUERY( "ts" << GTE << j ), 0, 0, 0, QueryOption_OplogReplay );
                    ASSERT( c->more() );
                    BSONObj next = c->next();
                    ASSERT( !next[ "ts" ].eoo() );
                    ASSERT_EQUALS( ( j > min ? j : min ), next[ "ts" ].numberInt() );
                }
                //cout << k << endl;
            }
        }
    };

    class FindingStartPartiallyFull : public CollectionBase {
    public:
        FindingStartPartiallyFull() : CollectionBase( "findingstart" ) {
        }

        void run() {
            size_t startNumCursors = numCursorsOpen();

            BSONObj info;
            ASSERT( client().runCommand( "unittests", BSON( "create" << "querytests.findingstart" << "capped" << true << "$nExtents" << 5 << "autoIndexId" << false ), info ) );

            int i = 0;
            for( ; i < 150; client().insert( ns(), BSON( "ts" << i++ ) ) );

            for( int k = 0; k < 5; ++k ) {
                client().insert( ns(), BSON( "ts" << i++ ) );
                int min = client().query( ns(), Query().sort( BSON( "$natural" << 1 ) ) )->next()[ "ts" ].numberInt();
                for( int j = -1; j < i; ++j ) {
                    auto_ptr< DBClientCursor > c = client().query( ns(), QUERY( "ts" << GTE << j ), 0, 0, 0, QueryOption_OplogReplay );
                    ASSERT( c->more() );
                    BSONObj next = c->next();
                    ASSERT( !next[ "ts" ].eoo() );
                    ASSERT_EQUALS( ( j > min ? j : min ), next[ "ts" ].numberInt() );
                }
            }

            ASSERT_EQUALS( startNumCursors, numCursorsOpen() );
        }
    };
    
    /**
     * Check OplogReplay mode where query timestamp is earlier than the earliest
     * entry in the collection.
     */
    class FindingStartStale : public CollectionBase {
    public:
        FindingStartStale() : CollectionBase( "findingstart" ) {}

        void run() {
            size_t startNumCursors = numCursorsOpen();

            // Check OplogReplay mode with missing collection.
            auto_ptr< DBClientCursor > c0 = client().query( ns(), QUERY( "ts" << GTE << 50 ), 0, 0, 0, QueryOption_OplogReplay );
            ASSERT( !c0->more() );

            BSONObj info;
            ASSERT( client().runCommand( "unittests", BSON( "create" << "querytests.findingstart" << "capped" << true << "$nExtents" << 5 << "autoIndexId" << false ), info ) );
            
            // Check OplogReplay mode with empty collection.
            auto_ptr< DBClientCursor > c = client().query( ns(), QUERY( "ts" << GTE << 50 ), 0, 0, 0, QueryOption_OplogReplay );
            ASSERT( !c->more() );

            // Check with some docs in the collection.
            for( int i = 100; i < 150; client().insert( ns(), BSON( "ts" << i++ ) ) );
            c = client().query( ns(), QUERY( "ts" << GTE << 50 ), 0, 0, 0, QueryOption_OplogReplay );
            ASSERT( c->more() );
            ASSERT_EQUALS( 100, c->next()[ "ts" ].numberInt() );

            // Check that no persistent cursors outlast our queries above.
            ASSERT_EQUALS( startNumCursors, numCursorsOpen() );
        }
    };

    class WhatsMyUri : public CollectionBase {
    public:
        WhatsMyUri() : CollectionBase( "whatsmyuri" ) {}
        void run() {
            BSONObj result;
            client().runCommand( "admin", BSON( "whatsmyuri" << 1 ), result );
            ASSERT_EQUALS( unknownAddress.toString(), result[ "you" ].str() );
        }
    };
    
    class CollectionInternalBase : public CollectionBase {
    public:
        CollectionInternalBase( const char *nsLeaf ) :
          CollectionBase( nsLeaf ),
          _lk(_txn.lockState(), ns() ),
          _ctx( ns() ) {
        }
    private:
        Lock::DBWrite _lk;
        Client::Context _ctx;
    };
    
    class Exhaust : public CollectionInternalBase {
    public:
        Exhaust() : CollectionInternalBase( "exhaust" ) {}
        void run() {
            BSONObj info;
            ASSERT( client().runCommand( "unittests",
                                        BSON( "create" << "querytests.exhaust" <<
                                             "capped" << true << "size" << 8192 ), info ) );
            client().insert( ns(), BSON( "ts" << 0 ) );
            Message message;
            assembleRequest( ns(), BSON( "ts" << GTE << 0 ), 0, 0, 0,
                            QueryOption_OplogReplay | QueryOption_CursorTailable |
                            QueryOption_Exhaust,
                            message );
            DbMessage dbMessage( message );
            QueryMessage queryMessage( dbMessage );
            Message result;
            string exhaust = newRunQuery( &_txn, message, queryMessage, *cc().curop(), result );
            ASSERT( exhaust.size() );
            ASSERT_EQUALS( string( ns() ), exhaust );
        }
    };

    class QueryCursorTimeout : public CollectionInternalBase {
    public:
        QueryCursorTimeout() : CollectionInternalBase( "querycursortimeout" ) {}
        void run() {
            for( int i = 0; i < 150; ++i ) {
                insert( ns(), BSONObj() );
            }
            auto_ptr<DBClientCursor> c = client().query( ns(), Query() );
            ASSERT( c->more() );
            long long cursorId = c->getCursorId();
            
            ClientCursor *clientCursor = 0;
            {
                Client::ReadContext ctx(&_txn, ns());
                ClientCursorPin clientCursorPointer( ctx.ctx().db()->getCollection( &_txn, ns() ),
                                                     cursorId );
                clientCursor = clientCursorPointer.c();
                // clientCursorPointer destructor unpins the cursor.
            }
            ASSERT( clientCursor->shouldTimeout( 600001 ) );
        }
    };

    class QueryReadsAll : public CollectionBase {
    public:
        QueryReadsAll() : CollectionBase( "queryreadsall" ) {}
        void run() {
            for( int i = 0; i < 5; ++i ) {
                insert( ns(), BSONObj() );
            }
            auto_ptr<DBClientCursor> c = client().query( ns(), Query(), 5 );
            ASSERT( c->more() );
            // With five results and a batch size of 5, no cursor is created.
            ASSERT_EQUALS( 0, c->getCursorId() );
        }
    };
    
    /**
     * Check that an attempt to kill a pinned cursor fails and produces an appropriate assertion.
     */
    class KillPinnedCursor : public CollectionBase {
    public:
        KillPinnedCursor() : CollectionBase( "killpinnedcursor" ) {
        }
        void run() {
            client().insert( ns(), vector<BSONObj>( 3, BSONObj() ) );
            auto_ptr<DBClientCursor> cursor = client().query( ns(), BSONObj(), 0, 0, 0, 0, 2 );
            ASSERT_EQUALS( 2, cursor->objsLeftInBatch() );
            long long cursorId = cursor->getCursorId();
            
            {
                Client::WriteContext ctx(&_txn,  ns() );
                ClientCursorPin pinCursor( ctx.ctx().db()->getCollection( &_txn, ns() ), cursorId );
 
                ASSERT_THROWS(CollectionCursorCache::eraseCursorGlobal(&_txn, cursorId),
                              MsgAssertionException);
                string expectedAssertion =
                        str::stream() << "Cannot kill active cursor " << cursorId;
                ASSERT_EQUALS( expectedAssertion, client().getLastError() );
            }
            
            // Verify that the remaining document is read from the cursor.
            ASSERT_EQUALS( 3, cursor->itcount() );
        }
    };

    namespace queryobjecttests {
        class names1 {
        public:
            void run() {
                ASSERT_EQUALS( BSON( "x" << 1 ) , QUERY( "query" << BSON( "x" << 1 ) ).getFilter() );
                ASSERT_EQUALS( BSON( "x" << 1 ) , QUERY( "$query" << BSON( "x" << 1 ) ).getFilter() );
            }

        };
    }

    class OrderingTest {
    public:
        void run() {
            {
                Ordering o = Ordering::make( BSON( "a" << 1 << "b" << -1 << "c" << 1 ) );
                ASSERT_EQUALS( 1 , o.get(0) );
                ASSERT_EQUALS( -1 , o.get(1) );
                ASSERT_EQUALS( 1 , o.get(2) );

                ASSERT( ! o.descending( 1 ) );
                ASSERT( o.descending( 1 << 1 ) );
                ASSERT( ! o.descending( 1 << 2 ) );
            }

            {
                Ordering o = Ordering::make( BSON( "a.d" << 1 << "a" << 1 << "e" << -1 ) );
                ASSERT_EQUALS( 1 , o.get(0) );
                ASSERT_EQUALS( 1 , o.get(1) );
                ASSERT_EQUALS( -1 , o.get(2) );

                ASSERT( ! o.descending( 1 ) );
                ASSERT( ! o.descending( 1 << 1 ) );
                ASSERT(  o.descending( 1 << 2 ) );
            }

        }
    };
    
    class All : public Suite {
    public:
        All() : Suite( "query" ) {
        }

        void setupTests() {
            add< FindingStart >();
            add< FindOneOr >();
            add< FindOneRequireIndex >();
            add< FindOneEmptyObj >();
            add< BoundedKey >();
            add< GetMore >();
            add< GetMoreKillOp >();
            add< GetMoreInvalidRequest >();
            add< PositiveLimit >();
            add< ReturnOneOfManyAndTail >();
            add< TailNotAtEnd >();
            add< EmptyTail >();
            add< TailableDelete >();
            add< TailableInsertDelete >();
            add< TailCappedOnly >();
            add< TailableQueryOnId >();
            add< OplogReplayMode >();
            add< OplogReplaySlaveReadTill >();
            add< OplogReplayExplain >();
            add< ArrayId >();
            add< UnderscoreNs >();
            add< EmptyFieldSpec >();
            add< MultiNe >();
            add< EmbeddedNe >();
            add< EmbeddedNumericTypes >();
            add< AutoResetIndexCache >();
            add< UniqueIndex >();
            add< UniqueIndexPreexistingData >();
            add< SubobjectInArray >();
            add< Size >();
            add< FullArray >();
            add< InsideArray >();
            add< IndexInsideArrayCorrect >();
            add< SubobjArr >();
            add< MinMax >();
            add< MatchCodeCodeWScope >();
            add< MatchDBRefType >();
            add< DirectLocking >();
            add< FastCountIn >();
            add< EmbeddedArray >();
            add< DifferentNumbers >();
            add< SymbolStringSame >();
            add< TailableCappedRaceCondition >();
            add< HelperTest >();
            add< HelperByIdTest >();
            add< FindingStartPartiallyFull >();
            add< FindingStartStale >();
            add< WhatsMyUri >();
            add< Exhaust >();
            add< QueryCursorTimeout >();
            add< QueryReadsAll >();
            add< KillPinnedCursor >();

            add< queryobjecttests::names1 >();

            add< OrderingTest >();
        }
    } myall;

} // namespace QueryTests

