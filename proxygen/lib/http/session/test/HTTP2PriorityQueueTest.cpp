/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include <list>
#include <map>
#include <proxygen/lib/http/session/HTTP2PriorityQueue.h>
#include <folly/io/async/test/UndelayedDestruction.h>

using namespace std::placeholders;
using folly::Random;

namespace {
static char* fakeTxn = (char*)0xface0000;

proxygen::HTTPTransaction* makeFakeTxn(proxygen::HTTPCodec::StreamID id) {
  return (proxygen::HTTPTransaction*)(fakeTxn + id);
}

proxygen::HTTPCodec::StreamID getTxnID(proxygen::HTTPTransaction* txn) {
  return (proxygen::HTTPCodec::StreamID)((char*)txn - fakeTxn);
}

// folly::Random::rand32 is broken because it takes RNG by value
uint32_t rand32(uint32_t max, folly::Random::DefaultGenerator& rng) {
    if (max == 0) {
      return 0;
    }

    return std::uniform_int_distribution<uint32_t>(0, max - 1)(rng);
  }

}

namespace proxygen {

typedef std::list<std::pair<HTTPCodec::StreamID, uint8_t>> IDList;

class QueueTest : public testing::Test {
 public:
  explicit QueueTest(folly::HHWheelTimer* timer=nullptr)
      : q_(timer) {
  }

 protected:
  void addTransaction(HTTPCodec::StreamID id, http2::PriorityUpdate pri,
                     bool pnode=false) {
    HTTP2PriorityQueue::Handle h =
      q_.addTransaction(id, pri, pnode ? nullptr : makeFakeTxn(id), false);
    handles_.insert(std::make_pair(id, h));
    if (!pnode) {
      signalEgress(id, 1);
    }
  }

  void removeTransaction(HTTPCodec::StreamID id) {
    q_.removeTransaction(handles_[id]);
  }

  void updatePriority(HTTPCodec::StreamID id, http2::PriorityUpdate pri) {
    handles_[id] = q_.updatePriority(handles_[id], pri);
  }

  void signalEgress(HTTPCodec::StreamID id, bool mark) {
    if (mark) {
      q_.signalPendingEgress(handles_[id]);
    } else {
      q_.clearPendingEgress(handles_[id]);
    }
  }

  void buildSimpleTree() {
    addTransaction(1, {0, false, 15});
    addTransaction(3, {1, false, 3});
    addTransaction(5, {1, false, 3});
    addTransaction(7, {1, false, 7});
    addTransaction(9, {5, false, 7});
  }

  bool visitNode(HTTPCodec::StreamID id, HTTPTransaction* txn, double r) {
    nodes_.push_back(std::make_pair(id, r * 100));
    return false;
  }

  void dump() {
    nodes_.clear();
    q_.iterate(std::bind(&QueueTest::visitNode, this, _1, _2, _3),
               [] { return false; }, true);
  }

  void dumpBFS(const std::function<bool()>& stopFn) {
    nodes_.clear();
    q_.iterateBFS(std::bind(&QueueTest::visitNode, this, _1, _2, _3),
                  stopFn, true);
  }

  void nextEgress() {
    HTTP2PriorityQueue::NextEgressResult nextEgressResults;
    q_.nextEgress(nextEgressResults);
    nodes_.clear();
    for (auto p: nextEgressResults) {
      nodes_.push_back(std::make_pair(getTxnID(p.first), p.second * 100));
    }
  }

  HTTP2PriorityQueue q_;
  std::map<HTTPCodec::StreamID, HTTP2PriorityQueue::Handle> handles_;
  IDList nodes_;
};


TEST_F(QueueTest, Basic) {
  buildSimpleTree();
  dump();
  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 25}, {5, 25}, {9, 100}, {7, 50}}));
}

TEST_F(QueueTest, RemoveLeaf) {
  buildSimpleTree();

  removeTransaction(3);
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 100}, {5, 33}, {9, 100}, {7, 66}}));
}

TEST_F(QueueTest, RemoveParent) {
  buildSimpleTree();

  removeTransaction(5);
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 25}, {7, 50}, {9, 25}}));
}

TEST_F(QueueTest, RemoveParentWeights) {
  // weight_ / totalChildWeight_ < 1
  addTransaction(1, {0, false, 0});
  addTransaction(3, {1, false, 255});
  addTransaction(5, {1, false, 255});

  removeTransaction(1);
  dump();

  EXPECT_EQ(nodes_, IDList({{3, 50}, {5, 50}}));
}

TEST_F(QueueTest, UpdateWeight) {
  buildSimpleTree();

  updatePriority(5, {1, false, 7});
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 20}, {5, 40}, {9, 100}, {7, 40}}));
}

TEST_F(QueueTest, UpdateWeightNotEnqueued) {
  addTransaction(1, {0, false, 7});
  addTransaction(3, {0, false, 7});

  signalEgress(1, false);
  signalEgress(3, false);
  updatePriority(1, {3, false, 7});
  dump();

  EXPECT_EQ(nodes_, IDList({{3, 100}, {1, 100}}));
}

TEST_F(QueueTest, UpdateWeightExcl) {
  buildSimpleTree();

  updatePriority(5, {1, true, 7});
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 100}, {5, 100}, {9, 40}, {3, 20}, {7, 40}}));
  signalEgress(1, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{5, 100}}));
}

TEST_F(QueueTest, UpdateWeightExclDequeued) {
  buildSimpleTree();

  signalEgress(5, false);
  updatePriority(5, {1, true, 7});
  signalEgress(1, false);
  nextEgress();

  EXPECT_EQ(nodes_, IDList({{9, 40}, {7, 40}, {3, 20}}));
}

TEST_F(QueueTest, UpdateParentSibling) {
  buildSimpleTree();

  updatePriority(5, {3, false, 3});
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 33}, {5, 100},
                               {9, 100}, {7, 66}}));
  signalEgress(1, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 66}, {3, 33}}));

  // Clear 5's egress (so it is only in the tree because 9 has egress) and move
  // it back.  Hit's a slightly different code path in reparent
  signalEgress(5, false);
  updatePriority(5, {1, false, 3});
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 25}, {7, 50}, {5, 25}, {9, 100}}));

  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 50}, {3, 25}, {9, 25}}));
}

TEST_F(QueueTest, UpdateParentSiblingExcl) {
  buildSimpleTree();

  updatePriority(7, {5, true, 3});
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 50}, {5, 50},
                              {7, 100}, {9, 100}}));
  signalEgress(1, false);
  signalEgress(3, false);
  signalEgress(5, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 100}}));
}

TEST_F(QueueTest, UpdateParentAncestor) {
  buildSimpleTree();

  updatePriority(9, {0, false, 15});
  dump();

  EXPECT_EQ(nodes_, IDList({{1, 50}, {3, 25}, {5, 25}, {7, 50}, {9, 50}}));
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{1, 50}, {9, 50}}));
}

TEST_F(QueueTest, UpdateParentAncestorExcl) {
  buildSimpleTree();

  updatePriority(9, {0, true, 15});
  dump();

  EXPECT_EQ(nodes_, IDList({{9, 100}, {1, 100}, {3, 25}, {5, 25}, {7, 50}}));
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{9, 100}}));
}

TEST_F(QueueTest, UpdateParentDescendant) {
  buildSimpleTree();

  updatePriority(1, {5, false, 7});
  dump();

  EXPECT_EQ(nodes_, IDList({{5, 100}, {9, 50}, {1, 50}, {3, 33}, {7, 66}}));
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{5, 100}}));
  signalEgress(5, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{9, 50}, {1, 50}}));
}

TEST_F(QueueTest, UpdateParentDescendantExcl) {
  buildSimpleTree();

  updatePriority(1, {5, true, 7});
  dump();

  EXPECT_EQ(nodes_, IDList({{5, 100}, {1, 100}, {3, 20}, {7, 40}, {9, 40}}));
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{5, 100}}));
  signalEgress(5, false);
  signalEgress(1, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 40}, {9, 40}, {3, 20}}));
}

TEST_F(QueueTest, ExclusiveAdd) {
  buildSimpleTree();

  addTransaction(11, {1, true, 100});

  dump();
  EXPECT_EQ(nodes_, IDList({
        {1, 100}, {11, 100}, {3, 25}, {5, 25}, {9, 100}, {7, 50}
      }));
}

TEST_F(QueueTest, AddUnknown) {
  buildSimpleTree();

  addTransaction(11, {75, false, 15});

  dump();
  EXPECT_EQ(nodes_, IDList({
        {1, 50}, {3, 25}, {5, 25}, {9, 100}, {7, 50}, {11, 50}
      }));
}

TEST_F(QueueTest, AddMax) {
  addTransaction(1, {0, false, 255});

  nextEgress();
  EXPECT_EQ(nodes_, IDList({{1, 100}}));
}

TEST_F(QueueTest, Misc) {
  buildSimpleTree();

  EXPECT_FALSE(q_.empty());
  EXPECT_EQ(q_.numPendingEgress(), 5);
  signalEgress(1, false);
  EXPECT_EQ(q_.numPendingEgress(), 4);
  EXPECT_FALSE(q_.empty());
  removeTransaction(9);
  removeTransaction(1);
  dump();
  EXPECT_EQ(nodes_, IDList({{3, 25}, {5, 25}, {7, 50}}));
}

TEST_F(QueueTest, iterateBFS) {
  buildSimpleTree();

  auto stopFn = [this] {
    return nodes_.size() > 2;
  };

  dumpBFS(stopFn);
  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 25}, {5, 25}, {7, 50}}));
}

TEST_F(QueueTest, nextEgress) {
  buildSimpleTree();

  nextEgress();
  EXPECT_EQ(nodes_, IDList({{1, 100}}));

  addTransaction(11, {7, false, 15});
  signalEgress(1, false);

  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 50}, {3, 25}, {5, 25}}));

  signalEgress(5, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 50}, {3, 25}, {9, 25}}));
  signalEgress(5, true);

  signalEgress(3, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 66}, {5, 33}}));

  signalEgress(5, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 66}, {9, 33}}));

  signalEgress(7, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{11, 66}, {9, 33}}));

  signalEgress(9, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{11, 100}}));

  signalEgress(3, true);
  signalEgress(7, true);
  signalEgress(9, true);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 50}, {3, 25}, {9, 25}}));
}

TEST_F(QueueTest, nextEgressExclusiveAdd) {
  buildSimpleTree();

  // clear all egress
  signalEgress(1, false);
  signalEgress(3, false);
  signalEgress(5, false);
  signalEgress(7, false);
  signalEgress(9, false);

  // Add a transaction with exclusive dependency, clear its egress
  addTransaction(11, {1, true, 100});
  signalEgress(11, false);

  // signal egress for a child that got moved via exclusive dep
  signalEgress(3, true);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{3, 100}}));
  EXPECT_EQ(q_.numPendingEgress(), 1);
}

TEST_F(QueueTest, nextEgressExclusiveAddWithEgress) {
  buildSimpleTree();

  // clear all egress, except 3
  signalEgress(1, false);
  signalEgress(5, false);
  signalEgress(7, false);
  signalEgress(9, false);

  // Add a transaction with exclusive dependency, clear its egress
  addTransaction(11, {1, true, 100});
  signalEgress(11, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{3, 100}}));
  EXPECT_EQ(q_.numPendingEgress(), 1);
}

TEST_F(QueueTest, nextEgressRemoveParent) {
  buildSimpleTree();

  // Clear egress for all except txn=9
  signalEgress(1, false);
  signalEgress(3, false);
  signalEgress(5, false);
  signalEgress(7, false);

  // Remove parent of 9 (5)
  removeTransaction(5);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{9, 100}}));

  // signal egress for 9's new siblings to verify weights
  signalEgress(3, true);
  signalEgress(7, true);

  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 50}, {9, 25}, {3, 25}}));
}

TEST_F(QueueTest, addExclusiveDescendantEnqueued) {
  addTransaction(1, {0, false, 100});
  addTransaction(3, {1, false, 100});
  addTransaction(5, {3, false, 100});
  signalEgress(1, false);
  signalEgress(3, false);
  // add a new exclusive child of 1.  1's child 3 is not enqueued but is in the
  // the egress tree.
  addTransaction(7, {1, true, 100});
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 100}}));
}

TEST_F(QueueTest, nextEgressRemoveParentEnqueued) {
  addTransaction(1, {0, false, 100});
  addTransaction(3, {1, false, 100});
  addTransaction(5, {3, false, 100});
  signalEgress(3, false);
  // When 3's children (5) are added to 1, both are already in the egress tree
  // and the signal does not need to propagate
  removeTransaction(3);
  signalEgress(1, false);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{5, 100}}));
}

TEST_F(QueueTest, nextEgressRemoveParentEnqueuedIndirect) {
  addTransaction(1, {0, false, 100});
  addTransaction(3, {1, false, 100});
  addTransaction(5, {3, false, 100});
  addTransaction(7, {1, false, 100});
  signalEgress(3, false);
  signalEgress(1, false);
  // When 3's children (5) are added to 1, both are already in the egress tree
  // and the signal does not need to propagate
  removeTransaction(3);
  nextEgress();
  EXPECT_EQ(nodes_, IDList({{7, 50}, {5, 50}}));
}

TEST_F(QueueTest, ChromeTest) {
  // Tries to simulate Chrome's current behavior by performing pseudo-random
  // add-exclusive, signal, clear and remove with 3 insertion points
  // (hi,mid,low).  Note the test uses rand32() with a particular seed so the
  // output is predictable.
  HTTPCodec::StreamID pris[3] = {1, 3, 5};
  addTransaction(1, {0, true, 99});
  signalEgress(1, false);
  addTransaction(3, {1, true, 99});
  signalEgress(3, false);
  addTransaction(5, {3, true, 99});
  signalEgress(5, false);

  std::vector<HTTPCodec::StreamID> txns;
  std::vector<HTTPCodec::StreamID> active;
  std::vector<HTTPCodec::StreamID> inactive;
  HTTPCodec::StreamID txn = 0;
  uint64_t idx = 0;
  HTTPCodec::StreamID nextId = 7;
  auto gen = Random::create();
  gen.seed(12345); // luggage combo
  for (auto i = 4; i < 1000; i++) {
    uint8_t action = rand32(4, gen);
    if (action == 0) {
      // add exclusive on pseudo-random priority anchor
      uint8_t pri = rand32(3, gen);
      HTTPCodec::StreamID dep = pris[pri];
      txn = nextId;
      nextId += 2;
      VLOG(2) << "Adding txn=" << txn << " with dep=" << dep;
      addTransaction(txn, {dep, true, 99});
      txns.push_back(txn);
      active.push_back(txn);
    } else if (action == 1 && !inactive.empty()) {
      // signal an inactive txn
      idx = rand32(inactive.size(), gen);
      txn = inactive[idx];
      VLOG(2) << "Activating txn=" << txn;
      signalEgress(txn, true);
      inactive.erase(inactive.begin() + idx);
      active.push_back(txn);
    } else if (action == 2 && !active.empty()) {
      // clear an active transaction
      idx = rand32(active.size(), gen);
      txn = active[idx];
      VLOG(2) << "Deactivating txn=" << txn;
      signalEgress(txn, false);
      active.erase(active.begin() + idx);
      inactive.push_back(txn);
    } else if (action == 3 && !txns.empty()) {
      // remove a transaction
      idx = rand32(txns.size(), gen);
      txn = txns[idx];
      VLOG(2) << "Removing txn=" << txn;
      removeTransaction(txn);
      txns.erase(txns.begin() + idx);
      auto it = std::find(active.begin(), active.end(), txn);
      if (it != active.end()) {
        active.erase(it);
      }
      it = std::find(inactive.begin(), inactive.end(), txn);
      if (it != inactive.end()) {
        inactive.erase(it);
      }
    }
    VLOG(2) << "Active nodes=" << q_.numPendingEgress();
    if (!q_.empty()) {
      nextEgress();
      EXPECT_GT(nodes_.size(), 0);
    }

  }
}

class DanglingQueueTest : public QueueTest {
 public:
  DanglingQueueTest() :
      QueueTest(&timer_) {
    HTTP2PriorityQueue::setNodeLifetime(std::chrono::milliseconds(30));
    timer_.setCatchupEveryN(1);
  }

  void expireNodes() {
    eventBase_.runAfterDelay([&] {
        eventBase_.terminateLoopSoon();
      }, 45);
    eventBase_.loop();
  }

 protected:

  folly::EventBase eventBase_;
  folly::UndelayedDestruction<folly::HHWheelTimer> timer_{&eventBase_};
};

TEST_F(DanglingQueueTest, basic) {
  addTransaction(1, {0, false, 15});
  removeTransaction(1);
  dump();
  EXPECT_EQ(nodes_, IDList({{1, 100}}));
  expireNodes();
  dump();
  EXPECT_EQ(nodes_, IDList({}));
}

TEST_F(DanglingQueueTest, chain) {
  addTransaction(1, {0, false, 15}, true);
  addTransaction(3, {1, false, 15}, true);
  addTransaction(5, {3, false, 15}, true);
  dump();
  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 100}, {5, 100}}));
  expireNodes();
  dump();
  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 100}}));
  expireNodes();
  dump();
  EXPECT_EQ(nodes_, IDList({{1, 100}}));
  expireNodes();
  dump();
  EXPECT_EQ(nodes_, IDList({}));
}

TEST_F(DanglingQueueTest, drop) {
  addTransaction(1, {0, false, 15}, true);
  addTransaction(3, {0, false, 15}, true);
  addTransaction(5, {1, false, 15}, true);
  dump();
  q_.dropPriorityNodes();
  dump();
  EXPECT_EQ(nodes_, IDList({}));
}

TEST_F(DanglingQueueTest, refresh) {
  addTransaction(1, {0, false, 15});
  addTransaction(3, {0, false, 15});
  // 1 is now virtual
  removeTransaction(1);
  dump();
  EXPECT_EQ(nodes_, IDList({{1, 50}, {3, 50}}));
  // before 1 times out, change it's priority, should still be there
  eventBase_.runAfterDelay([&] {
      updatePriority(1, {0, false, 3});
      dump();
      EXPECT_EQ(nodes_, IDList({{1, 20}, {3, 80}}));
    }, 20);

  expireNodes();
  dump();
  EXPECT_EQ(nodes_, IDList({{1, 20}, {3, 80}}));
  expireNodes();
  dump();
  EXPECT_EQ(nodes_, IDList({{3, 100}}));
}

TEST_F(DanglingQueueTest, max) {
  buildSimpleTree();
  q_.setMaxVirtualNodes(3);
  for (auto i = 1; i <= 9; i += 2) {
    removeTransaction(i);
  }

  dump();
  EXPECT_EQ(nodes_, IDList({{1, 100}, {3, 50}, {5, 50}}));
  expireNodes();
  dump();
  EXPECT_EQ(nodes_, IDList());
}

}
