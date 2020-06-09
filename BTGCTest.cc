// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdlib>
#include <iostream>
#include <random>

#define __BTGC_Malloc__ testMalloc
#define __BTGC_Free__ testFree

int totalObjects = 0;

void* testMalloc(size_t size) {
  ++totalObjects;
  return malloc(size);
}

void testFree(void* ptr) {
  --totalObjects;
  free(ptr);
}

#include "BTGC.h"

using namespace btgc;

struct Thing {
  Ptr<Thing> next;
  Thing() : next(this) {}  // Initialize non-root Ptr with this.
};

int main() {
  BTGC::init();

  constexpr int kIterations = 1000000;
  constexpr int kReportInterval = 1000;
  constexpr int kThingLinks = 10;
  constexpr int kTargetRoots = 100;

  std::minstd_rand0 generator(time(nullptr));
  std::uniform_int_distribution<int> randThing(0, 2 * kTargetRoots - 1);

  // Array of Things, each of which is the head of a list of 10 Things.
  Array<Ptr<Thing>> things;
  for (int i = 0; i < kIterations; ++i) {
    Ptr<Thing> t;
    for (int j = 0; j < kThingLinks; ++j) {
      Ptr<Thing> u = Ptr<Thing>::make();
      u->next = t;
      t = u;
    }
    things.add(t);
    for (int j = 0; j < 2; ++j) {
      int r = randThing(generator);
      if (r < things.size()) {
        things.del(r);
        things[things.size()] = nullptr;
      }
    }
    if (i % kReportInterval == 0) {
      int reachables = things.size() * kThingLinks;
      std::cout << "Iteration: " << i << "\tReachable objects: " << reachables
                << "\tTotal objects: " << totalObjects << "\tWaste: "
                << ((totalObjects - reachables) * 100.0 / reachables) << "%"
                << std::endl;
    }
  }

  BTGC::finish();

  if (totalObjects != 0) {
    std::cerr << "Cleanup failed. Leaked: " << totalObjects << std::endl;
    return 1;
  }
  return 0;
}
