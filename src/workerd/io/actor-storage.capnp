# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xb200a391b94343f1;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::rpc");
$Cxx.allowCancellation;

interface ActorStorage @0xd7759d7fc87c08e4 {
  struct KeyValue {
    key @0 :Data;
    value @1 :Data;
  }

  struct KeyRename {
    oldKey @0 :Data;
    newKey @1 :Data;
  }

  getStage @0 (stableId :Text) -> (stage :Stage);
  # Get the storage capability for the given stage of the pipeline, identified by its stable ID.

  interface Operations @0xb512f2ce1f544439 {
    # These operations are tagged with an ExpiryMsSinceUnixEpoch timestamp.
    # This optional timestamp can be used to set a timeout after which the storage operation
    # should no longer be executed, and an exception should be thrown instead.
    get @0 (key :Data, ExpiryMsSinceUnixEpoch :UInt64) -> (value :Data);
    list @3 (start :Data, end :Data, limit :Int32, reverse :Bool, stream :ListStream, prefix :Data, ExpiryMsSinceUnixEpoch :UInt64);
    put @1 (entries :List(KeyValue), ExpiryMsSinceUnixEpoch :UInt64);
    delete @2 (keys :List(Data), ExpiryMsSinceUnixEpoch :UInt64) -> (numDeleted :Int32);

    getMultiple @4 (keys :List(Data), stream :ListStream, ExpiryMsSinceUnixEpoch :UInt64);
    deleteAll @5 (ExpiryMsSinceUnixEpoch :UInt64) -> (numDeleted :Int32);

    rename @9 (entries :List(KeyRename), ExpiryMsSinceUnixEpoch :UInt64) -> (renamed :List(Data));

    getAlarm @6 (ExpiryMsSinceUnixEpoch :UInt64) -> (scheduledTimeMs :Int64);
    setAlarm @7 (scheduledTimeMs :Int64, ExpiryMsSinceUnixEpoch :UInt64);
    deleteAlarm @8 (timeToDeleteMs :Int64, ExpiryMsSinceUnixEpoch :UInt64) -> (deleted :Bool);
  }

  struct DbSettings {
    enum Priority {
      default @0;
      low @1;
    }
    priority @0 :Priority;
    asOfTimeMs @1 :Int64;
  }

  interface Stage @0xdc35f52864c57550 extends(Operations) {
    txn @0 (settings :DbSettings) -> (transaction :Transaction);

    interface Transaction extends(Operations) {
      commit @0 (ExpiryMsSinceUnixEpoch :UInt64);
      rollback @1 ();
    }
  }

  interface ListStream {
    values @0 (list :List(KeyValue)) -> stream;
    end @1 ();
  }

  const maxKeys :UInt32 = 128;
  # The maximum number of keys that clients should be allowed to modify in a single storage
  # operation. This should be enforced for operations that access or modify multiple keys. This
  # limit will not be enforced upon the total count of keys involved in explicit transactions.

  const renameLimit :UInt32 = 1000;
  # The maximum number of keys in a rename() operation.
}
