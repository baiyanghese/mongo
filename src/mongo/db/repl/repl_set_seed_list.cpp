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

#include "mongo/db/repl/repl_set_seed_list.h"

#include <boost/thread.hpp>
#include <iostream>

#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/rs.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

    /** @param cfgString <setname>/<seedhost1>,<seedhost2> */
    void parseReplSetSeedList(const std::string& cfgString,
                              std::string& setname,
                              std::vector<HostAndPort>& seeds,
                              std::set<HostAndPort>& seedSet) {
        const char *p = cfgString.c_str();
        const char *slash = strchr(p, '/');
        if( slash )
            setname = string(p, slash-p);
        else
            setname = p;
        uassert(13093,
                "bad --replSet config string format is: <setname>[/<seedhost1>,<seedhost2>,...]",
                !setname.empty());

        if( slash == 0 )
            return;

        p = slash + 1;
        while( 1 ) {
            const char *comma = strchr(p, ',');
            if( comma == 0 ) comma = strchr(p,0);
            if( p == comma )
                break;
            {
                HostAndPort m;
                try {
                    m = HostAndPort( string(p, comma-p) );
                }
                catch(...) {
                    uassert(13114, "bad --replSet seed hostname", false);
                }
                uassert(13096, "bad --replSet command line config string - dups?",
                        seedSet.count(m) == 0);
                seedSet.insert(m);
                //uassert(13101, "can't use localhost in replset host list", !m.isLocalHost());
                if (isSelf(m)) {
                    LOG(1) << "replSet ignoring seed " << m.toString() << " (=self)" << rsLog;
                }
                else
                    seeds.push_back(m);
                if( *comma == 0 )
                    break;
                p = comma + 1;
            }
        }
    }

} // namespace repl
} // namespace mongo
