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

#include <sstream>
#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/instance.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/operation_context_impl.h"

namespace mongo {
    class ApplyOpsCmd : public Command {
    public:
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return true; }

        ApplyOpsCmd() : Command( "applyOps" ) {}
        virtual void help( stringstream &help ) const {
            help << "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , res : ... } ] }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // applyOps can do pretty much anything, so require all privileges.
            RoleGraph::generateUniversalPrivileges(out);
        }
        virtual bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

            if ( cmdObj.firstElement().type() != Array ) {
                errmsg = "ops has to be an array";
                return false;
            }

            BSONObj ops = cmdObj.firstElement().Obj();

            {
                // check input
                BSONObjIterator i( ops );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() == Object )
                        continue;
                    errmsg = "op not an object: ";
                    errmsg += e.fieldName();
                    return false;
                }
            }

            // SERVER-4328 todo : is global ok or does this take a long time? i believe multiple 
            // ns used so locking individually requires more analysis
            Lock::GlobalWrite globalWriteLock(txn->lockState());

            DBDirectClient db(txn);

            // Preconditions check reads the database state, so needs to be done locked
            if ( cmdObj["preCondition"].type() == Array ) {
                BSONObjIterator i( cmdObj["preCondition"].Obj() );
                while ( i.more() ) {
                    BSONObj f = i.next().Obj();

                    DBDirectClient db( txn );
                    BSONObj realres = db.findOne( f["ns"].String() , f["q"].Obj() );

                    // Apply-ops would never have a $where matcher, so use the default callback,
                    // which will throw an error if $where is found.
                    Matcher m(f["res"].Obj());
                    if ( ! m.matches( realres ) ) {
                        result.append( "got" , realres );
                        result.append( "whatFailed" , f );
                        errmsg = "pre-condition failed";
                        return false;
                    }
                }
            }

            // apply
            int num = 0;
            int errors = 0;
            
            BSONObjIterator i( ops );
            BSONArrayBuilder ab;
            const bool alwaysUpsert = cmdObj.hasField("alwaysUpsert") ?
                    cmdObj["alwaysUpsert"].trueValue() : true;
            
            while ( i.more() ) {
                BSONElement e = i.next();
                const BSONObj& temp = e.Obj();

                string ns = temp["ns"].String();

                // Run operations under a nested lock as a hack to prevent them from yielding.
                //
                // The list of operations is supposed to be applied atomically; yielding would break
                // atomicity by allowing an interruption or a shutdown to occur after only some
                // operations are applied.  We are already locked globally at this point, so taking
                // a DBWrite on the namespace creates a nested lock, and yields are disallowed for
                // operations that hold a nested lock.
                Lock::DBWrite lk(txn->lockState(), ns);
                invariant(txn->lockState()->isRecursive());

                Client::Context ctx(ns);
                bool failed = repl::applyOperation_inlock(txn,
                                                             ctx.db(),
                                                             temp,
                                                             false,
                                                             alwaysUpsert);
                ab.append(!failed);
                if ( failed )
                    errors++;

                num++;

                logOpForDbHash(ns.c_str());
            }

            result.append( "applied" , num );
            result.append( "results" , ab.arr() );

            if ( ! fromRepl ) {
                // We want this applied atomically on slaves
                // so we re-wrap without the pre-condition for speed

                string tempNS = str::stream() << dbname << ".$cmd";

                // TODO: possibly use mutable BSON to remove preCondition field
                // once it is available
                BSONObjIterator iter(cmdObj);
                BSONObjBuilder cmdBuilder;

                while (iter.more()) {
                    BSONElement elem(iter.next());
                    if (strcmp(elem.fieldName(), "preCondition") != 0) {
                        cmdBuilder.append(elem);
                    }
                }

                repl::logOp(txn, "c", tempNS.c_str(), cmdBuilder.done());
            }

            return errors == 0;
        }
    } applyOpsCmd;

}
