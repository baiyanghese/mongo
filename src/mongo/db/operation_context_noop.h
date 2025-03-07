/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"

#include "mongo/db/storage/recovery_unit_noop.h"
#pragma once

namespace mongo {

    class OperationContextNoop : public OperationContext {
    public:
        OperationContextNoop(RecoveryUnit* ru) {
            _recoveryUnit.reset(ru);
        }

        OperationContextNoop() {
            _recoveryUnit.reset(new RecoveryUnitNoop());
        }

        virtual ~OperationContextNoop() { }

        virtual Client* getClient() const {
            invariant(false);
            return NULL;
        }

        virtual CurOp* getCurOp() const {
            invariant(false);
            return NULL;
        }

        virtual RecoveryUnit* recoveryUnit() const {
            return _recoveryUnit.get();
        }

        virtual LockState* lockState() const {
            // TODO: Eventually, this should return an actual LockState object. For now,
            //       LockState depends on the whole world and is not necessary for testing.
            return NULL;
        }

        virtual ProgressMeter* setMessage(const char * msg,
                                          const std::string &name,
                                          unsigned long long progressMeterTotal,
                                          int secondsBetween) {
            invariant(false);
            return NULL;
        }

        virtual void checkForInterrupt(bool heedMutex = true) const { }

        virtual Status checkForInterruptNoAssert() const {
            return Status::OK();
        }

        virtual bool isPrimaryFor( const StringData& ns ) {
            return true;
        }

        virtual const char * getNS() const {
            return NULL;
        };

        virtual Transaction* getTransaction() {
            return NULL;
        }

    private:
        boost::scoped_ptr<RecoveryUnit> _recoveryUnit;
    };

}  // namespace mongo
