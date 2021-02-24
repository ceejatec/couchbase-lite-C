//
// ReplicatorEETest.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "ReplicatorTest.hh"


#ifdef COUCHBASE_ENTERPRISE     // Local-to-local replication is an EE feature


class ReplicatorLocalTest : public ReplicatorTest {
public:
    Database otherDB;

    ReplicatorLocalTest()
    :otherDB(openEmptyDatabaseNamed("otherDB"))
    {
        config.endpoint = CBLEndpoint_NewWithLocalDB(otherDB.ref());
        config.replicatorType = kCBLReplicatorTypePush;
    }
};


TEST_CASE_METHOD(ReplicatorLocalTest, "Push to local db", "[Replicator]") {
    MutableDocument doc("foo");
    doc["greeting"] = "Howdy!";
    db.saveDocument(doc);

    replicate();

    CHECK(asVector(docsNotified) == vector<string>{"foo"});

    Document copiedDoc = otherDB.getDocument("foo");
    REQUIRE(copiedDoc);
    CHECK(copiedDoc["greeting"].asString() == "Howdy!"_sl);
}


TEST_CASE_METHOD(ReplicatorLocalTest, "Pull conflict (default resolver)", "[Replicator][Conflict]") {
    config.replicatorType = kCBLReplicatorTypePull;

    MutableDocument doc("foo");
    doc["greeting"] = "Howdy!";
    db.saveDocument(doc);

    MutableDocument doc2("foo");
    doc2["greeting"] = "Salaam Alaykum";
    otherDB.saveDocument(doc2);

    replicate();

    CHECK(asVector(docsNotified) == vector<string>{"foo"});

    Document copiedDoc = db.getDocument("foo");
    REQUIRE(copiedDoc);
    CHECK(copiedDoc["greeting"].asString() == "Howdy!"_sl);
}


class ReplicatorConflictTest : public ReplicatorLocalTest {
public:

    bool deleteLocal {false}, deleteRemote {false}, deleteMerged {false};
    bool resolverCalled {false};

    alloc_slice expectedLocalRevID, expectedRemoteRevID;

    void testConflict(bool delLocal, bool delRemote, bool delMerged) {
        deleteLocal = delLocal;
        deleteRemote = delRemote;
        deleteMerged = delMerged;

        config.replicatorType = kCBLReplicatorTypePull;

        // Save the same doc to each db (will have the same revision),
        MutableDocument doc("foo");
        doc["greeting"] = "Howdy!";
        db.saveDocument(doc);
        if (deleteLocal) {
            db.deleteDocument(doc);
        } else {
            doc["expletive"] = "Shazbatt!";
            db.saveDocument(doc);
            expectedLocalRevID = doc.revisionID();
        }

        doc = MutableDocument("foo");
        doc["greeting"] = "Howdy!";
        otherDB.saveDocument(doc);
        if (deleteRemote) {
            otherDB.deleteDocument(doc);
        } else {
            doc["expletive"] = "Frak!";
            otherDB.saveDocument(doc);
            expectedRemoteRevID = doc.revisionID();
        }

        config.conflictResolver = [](void *context,
                                     FLString documentID,
                                     const CBLDocument *localDocument,
                                     const CBLDocument *remoteDocument) -> const CBLDocument* {
            cerr << "--- Entering custom conflict resolver! (local=" << localDocument <<
                    ", remote=" << remoteDocument << ")\n";
            auto merged = ((ReplicatorConflictTest*)context)->conflictResolver(documentID, localDocument, remoteDocument);
            cerr << "--- Returning " << merged << " from custom conflict resolver\n";
            return merged;
        };

        replicate();

        CHECK(resolverCalled);
        CHECK(asVector(docsNotified) == vector<string>{"foo"});

        Document copiedDoc = db.getDocument("foo");
        if (deleteMerged) {
            REQUIRE(!copiedDoc);
        } else {
            REQUIRE(copiedDoc);
            CHECK(copiedDoc["greeting"].asString() == "¡Hola!"_sl);
        }
    }


    const CBLDocument* conflictResolver(slice documentID,
                                        const CBLDocument *localDocument,
                                        const CBLDocument *remoteDocument)
    {
        CHECK(!resolverCalled);
        resolverCalled = true;

        CHECK(string(documentID) == "foo");
        if (deleteLocal) {
            REQUIRE(!localDocument);
            REQUIRE(!expectedLocalRevID);
        } else {
            REQUIRE(localDocument);
            CHECK(CBLDocument_ID(localDocument) == "foo"_sl);
            CHECK(slice(CBLDocument_RevisionID(localDocument)) == expectedLocalRevID);
            Dict localProps(CBLDocument_Properties(localDocument));
            CHECK(localProps["greeting"].asString() == "Howdy!"_sl);
            CHECK(localProps["expletive"].asString() == "Shazbatt!"_sl);
        }
        if (deleteRemote) {
            REQUIRE(!remoteDocument);
            REQUIRE(!expectedRemoteRevID);
        } else {
            REQUIRE(remoteDocument);
            CHECK(CBLDocument_ID(remoteDocument) == "foo"_sl);
            CHECK(slice(CBLDocument_RevisionID(remoteDocument)) == expectedRemoteRevID);
            Dict remoteProps(CBLDocument_Properties(remoteDocument));
            CHECK(remoteProps["greeting"].asString() == "Howdy!"_sl);
            CHECK(remoteProps["expletive"].asString() == "Frak!"_sl);
        }
        if (deleteMerged) {
            return nullptr;
        } else {
            CBLDocument *merged = CBLDocument_NewWithID(documentID);
            MutableDict mergedProps(CBLDocument_MutableProperties(merged));
            mergedProps.set("greeting"_sl, "¡Hola!");
            // do not release `merged`, otherwise it would be freed before returning!
            return merged;
        }
    }
};


TEST_CASE_METHOD(ReplicatorConflictTest, "Pull conflict (custom resolver)",
                 "[Replicator][Conflict]") {
    testConflict(false, false, false);
}


TEST_CASE_METHOD(ReplicatorConflictTest, "Pull conflict with remote deletion (custom resolver)",
                 "[Replicator][Conflict]") {
    testConflict(false, true, false);
}


TEST_CASE_METHOD(ReplicatorConflictTest, "Pull conflict with local deletion (custom resolver)",
                 "[Replicator][Conflict]") {
    testConflict(true, false, false);
}


TEST_CASE_METHOD(ReplicatorConflictTest, "Pull conflict deleting merge (custom resolver)",
                 "[Replicator][Conflict]") {
    testConflict(false, true, true);
}


#endif // COUCHBASE_ENTERPRISE
