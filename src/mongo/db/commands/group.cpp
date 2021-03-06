// group.cpp

/**
*    Copyright (C) 2012-2014 MongoDB Inc.
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

#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/instance.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/scripting/engine.h"

namespace mongo {

    class GroupCommand : public Command {
    public:
        GroupCommand() : Command("group") {}
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool slaveOk() const { return false; }
        virtual bool slaveOverrideOk() const { return true; }
        virtual void help( stringstream &help ) const {
            help << "http://dochub.mongodb.org/core/aggregation";
        }
        Status checkAuthForCommand(ClientBasic* client,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj) {
            std::string ns = parseNs(dbname, cmdObj);
            if (!client->getAuthorizationSession()->isAuthorizedForActionsOnNamespace(
                    NamespaceString(ns), ActionType::find)) {
                return Status(ErrorCodes::Unauthorized, "unauthorized");
            }
            return Status::OK();
        }

        string parseNs(const string& dbname, const BSONObj& cmdObj) const {
            const BSONObj& p = cmdObj.firstElement().embeddedObjectUserCheck();
            uassert(17211, "ns has to be set", p["ns"].type() == String);
            return dbname + "." + p["ns"].String();
        }

        BSONObj getKey( const BSONObj& obj , const BSONObj& keyPattern , ScriptingFunction func , double avgSize , Scope * s ) {
            if ( func ) {
                BSONObjBuilder b( obj.objsize() + 32 );
                b.append( "0" , obj );
                const BSONObj& key = b.obj();
                int res = s->invoke( func , &key, 0 );
                uassert( 10041 ,  (string)"invoke failed in $keyf: " + s->getError() , res == 0 );
                int type = s->type("__returnValue");
                uassert( 10042 ,  "return of $key has to be an object" , type == Object );
                return s->getObject( "__returnValue" );
            }
            return obj.extractFields( keyPattern , true ).getOwned();
        }

        bool group( OperationContext* txn,
                    Database* db,
                    const std::string& ns,
                    const BSONObj& query,
                    BSONObj keyPattern,
                    const std::string& keyFunctionCode,
                    const std::string& reduceCode,
                    const char * reduceScope,
                    BSONObj initial,
                    const std::string& finalize,
                    string& errmsg,
                    BSONObjBuilder& result ) {

            const string userToken = ClientBasic::getCurrent()->getAuthorizationSession()
                                                              ->getAuthenticatedUserNamesToken();
            auto_ptr<Scope> s = globalScriptEngine->getPooledScope(db->name(), "group" + userToken);

            if ( reduceScope )
                s->init( reduceScope );

            s->setObject( "$initial" , initial , true );

            s->exec( "$reduce = " + reduceCode , "$group reduce setup" , false , true , true , 100 );
            s->exec( "$arr = [];" , "$group reduce setup 2" , false , true , true , 100 );
            ScriptingFunction f = s->createFunction(
                                      "function(){ "
                                      "  if ( $arr[n] == null ){ "
                                      "    next = {}; "
                                      "    Object.extend( next , $key ); "
                                      "    Object.extend( next , $initial , true ); "
                                      "    $arr[n] = next; "
                                      "    next = null; "
                                      "  } "
                                      "  $reduce( obj , $arr[n] ); "
                                      "}" );

            ScriptingFunction keyFunction = 0;
            if ( keyFunctionCode.size() ) {
                keyFunction = s->createFunction( keyFunctionCode.c_str() );
            }


            double keysize = keyPattern.objsize() * 3;
            double keynum = 1;

            Collection* collection = db->getCollection( txn, ns );

            const WhereCallbackReal whereCallback(txn, StringData(db->name()));

            map<BSONObj,int,BSONObjCmp> map;
            list<BSONObj> blah;

            if (collection) {
                CanonicalQuery* cq;
                if (!CanonicalQuery::canonicalize(ns, query, &cq, whereCallback).isOK()) {
                    uasserted(17212, "Can't canonicalize query " + query.toString());
                    return 0;
                }

                PlanExecutor* rawExec;
                if (!getExecutor(txn,collection, cq, &rawExec).isOK()) {
                    uasserted(17213, "Can't get executor for query " + query.toString());
                    return 0;
                }

                auto_ptr<PlanExecutor> exec(rawExec);
                const ScopedExecutorRegistration safety(exec.get());

                BSONObj obj;
                Runner::RunnerState state;
                while (Runner::RUNNER_ADVANCED == (state = exec->getNext(&obj, NULL))) {
                    BSONObj key = getKey(obj , keyPattern , keyFunction , keysize / keynum,
                                         s.get() );
                    keysize += key.objsize();
                    keynum++;

                    int& n = map[key];
                    if ( n == 0 ) {
                        n = map.size();
                        s->setObject( "$key" , key , true );
                        uassert(17203, "group() can't handle more than 20000 unique keys",
                                n <= 20000 );
                    }

                    s->setObject( "obj" , obj , true );
                    s->setNumber( "n" , n - 1 );
                    if ( s->invoke( f , 0, 0 , 0 , true ) ) {
                        throw UserException(17214,
                                            (string)"reduce invoke failed: " + s->getError());
                    }
                }
            }

            if (!finalize.empty()) {
                s->exec( "$finalize = " + finalize , "$group finalize define" ,
                         false , true , true , 100 );
                ScriptingFunction g = s->createFunction(
                                          "function(){ "
                                          "  for(var i=0; i < $arr.length; i++){ "
                                          "  var ret = $finalize($arr[i]); "
                                          "  if (ret !== undefined) "
                                          "    $arr[i] = ret; "
                                          "  } "
                                          "}" );
                s->invoke( g , 0, 0 , 0 , true );
            }

            result.appendArray( "retval" , s->getObject( "$arr" ) );
            result.append( "count" , keynum - 1 );
            result.append( "keys" , (int)(map.size()) );
            s->exec( "$arr = [];" , "$group reduce setup 2" , false , true , true , 100 );
            s->gc();

            return true;
        }

        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            if ( !globalScriptEngine ) {
                errmsg = "server-side JavaScript execution is disabled";
                return false;
            }
            
            /* db.$cmd.findOne( { group : <p> } ) */
            const BSONObj& p = jsobj.firstElement().embeddedObjectUserCheck();

            BSONObj q;
            if ( p["cond"].type() == Object )
                q = p["cond"].embeddedObject();
            else if ( p["condition"].type() == Object )
                q = p["condition"].embeddedObject();
            else
                q = getQuery( p );

            BSONObj key;
            string keyf;
            if ( p["key"].type() == Object ) {
                key = p["key"].embeddedObjectUserCheck();
                if ( ! p["$keyf"].eoo() ) {
                    errmsg = "can't have key and $keyf";
                    return false;
                }
            }
            else if ( p["$keyf"].type() ) {
                keyf = p["$keyf"]._asCode();
            }
            else {
                // no key specified, will use entire object as key
            }

            BSONElement reduce = p["$reduce"];
            if ( reduce.eoo() ) {
                errmsg = "$reduce has to be set";
                return false;
            }

            BSONElement initial = p["initial"];
            if ( initial.type() != Object ) {
                errmsg = "initial has to be an object";
                return false;
            }


            string finalize;
            if (p["finalize"].type())
                finalize = p["finalize"]._asCode();

            const string ns = parseNs(dbname, jsobj);
            Client::ReadContext ctx(txn, ns);

            return group( txn, ctx.ctx().db() , ns , q ,
                          key , keyf , reduce._asCode() , reduce.type() != CodeWScope ? 0 : reduce.codeWScopeScopeDataUnsafe() ,
                          initial.embeddedObject() , finalize ,
                          errmsg , result );
        }

    } cmdGroup;


} // namespace mongo
