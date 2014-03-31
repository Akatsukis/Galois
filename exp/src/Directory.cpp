/** Galois Distributed Directory -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */

#include "Galois/Runtime/DistSupport.h"
#include "Galois/Runtime/Directory.h"
#include "Galois/Runtime/ll/TID.h"

#include <mutex>
#include <iostream>

using namespace Galois::Runtime;

////////////////////////////////////////////////////////////////////////////////

void RemoteDirectory::makeProgress() {
  //FIXME: write
}

void RemoteDirectory::dump() {
  //FIXME: write
  std::lock_guard<LL::SimpleLock> lg(md_lock);
  for(auto& pair : md) {
    std::lock_guard<LL::SimpleLock> mdlg(pair.second.lock);
    std::cout << pair.first << ": ";
    pair.second.dump(std::cout);
    std::cout << "\n";
  }
}

void RemoteDirectory::dump(fatPointer ptr) {
  //FIXME: write
}

void RemoteDirectory::setContended(fatPointer ptr) {
  //FIXME: write
  trace("RemoteDirectory::setContended %\n", ptr);
}

void RemoteDirectory::clearContended(fatPointer ptr) {
  //FIXME: write
  trace("RemoteDirectory::clearContended %\n", ptr);
}

RemoteDirectory::metadata* RemoteDirectory::getMD(fatPointer ptr) {
  std::lock_guard<LL::SimpleLock> lg(md_lock);
  assert(ptr.getHost() != NetworkInterface::ID);
  return &md[ptr];
}

//Recieve OK to upgrade RO -> RW
void RemoteDirectory::recvUpgrade(fatPointer ptr, typeHelper* th) {
  trace("RemoteDirectory::recvUpgrade %\n", ptr);
  metadata* md = getMD(ptr);
  std::lock_guard<LL::SimpleLock> lg(md->lock);
  assert(md->state == metadata::UPGRADE);
  md->state = metadata::HERE_RW;
}

//Recieve request to invalidate object
//FIXME: deal with cause and local priority control
void RemoteDirectory::recvInvalidate(fatPointer ptr, uint32_t cause, typeHelper* th) {
  trace("RemoteDirectory::recvInvalidate %\n", ptr);
  metadata* md = getMD(ptr);
  std::lock_guard<LL::SimpleLock> lg(md->lock); //FIXME: transfer lock
  assert(md->state == metadata::HERE_RO || md->state == metadata::HERE_RW);
  if (md->state == metadata::HERE_RW)
    writeback.push_back({ptr, md->obj});
  md->state = metadata::INVALID;
  md->obj = nullptr;
}

void RemoteDirectory::recvRequestImpl(fatPointer ptr, ResolveFlag flag, uint32_t cause, typeHelper* th) {
  switch (flag) {
  case INV:
    recvInvalidate(ptr, cause, th);
    break;
  case RW:
    recvUpgrade(ptr, th);
  default:
    abort();
  }
}

void RemoteDirectory::recvObjectImpl(fatPointer ptr, ResolveFlag flag, Lockable* obj) {
  trace("RemoteDirectory::recvObject % % %\n", ptr, flag, obj);
  metadata* md = getMD(ptr);
  std::lock_guard<LL::SimpleLock> lg(md->lock);
  assert(md->obj == nullptr);
  assert((md->state == metadata::PENDING_RO && flag == RO)
         || (md->state == metadata::PENDING_RW && flag == RW));
  md->state = flag == RW ? metadata::HERE_RW : metadata::HERE_RO;
  md->obj = obj;
}

//! precondition: md is locked
void RemoteDirectory::resolveNotPresent(fatPointer ptr, ResolveFlag flag, metadata* md, typeHelper* th) {
  if ((flag == RW && (md->state == metadata::UPGRADE || md->state == metadata::PENDING_RW)) ||
      (flag == RO && (md->state == metadata::PENDING_RO)))
    return;
  if (flag == RW) {
    switch(md->state) {
    case metadata::INVALID:
    case metadata::PENDING_RO:
      md->state = metadata::PENDING_RW;
      break;
    case metadata::PENDING_RW:
    case metadata::UPGRADE:
      return;
    default:
      abort();
    };
  } else if (flag == RO) {
    switch(md->state) {
    case metadata::INVALID:
      md->state = metadata::PENDING_RO;
      break;
    case metadata::PENDING_RO:
      return;
    default:
      abort();
    };
  }
  trace("RemoteDirectory::resolveNotPresent Sending Message % % %\n", ptr, flag, md->state);
  th->requestObj(ptr, flag);
}

void RemoteDirectory::metadata::dump(std::ostream& os) const {
  static const char* StateFlagNames[] = {"I", "PR", "PW", "RO", "RW", "UW"};
  os << "{" << StateFlagNames[state] << " " << obj << "}";
}



////////////////////////////////////////////////////////////////////////////////

LocalDirectory::metadata* LocalDirectory::getMD(Lockable* ptr) {
  std::lock_guard<LL::SimpleLock> lg(dir_lock);
  auto ii = dir.find(ptr);
  if (ii == dir.end())
    return nullptr;
  ii->second.lock.lock();
  return &ii->second;
}

LocalDirectory::metadata& LocalDirectory::getOrCreateMD(Lockable* ptr) {
  std::lock_guard<LL::SimpleLock> lg(dir_lock);
  metadata& md = dir[ptr];
  md.lock.lock();
  return md;
}


void LocalDirectory::recvRequestImpl(fatPointer ptr, ResolveFlag flag, uint32_t dest, typeHelper* th) {
  assert(ptr.getHost() == NetworkInterface::ID);
  trace("LocalDirectory::recvRequestImpl % % %\n", ptr, flag, dest);
  metadata& md = getOrCreateMD(static_cast<Lockable*>(ptr.getObj()));
  std::lock_guard<LL::SimpleLock> lg(md.lock, std::adopt_lock);
  md.t = th;
  switch(flag) {
  case RW:
    md.reqsRW.insert(dest);
    break;
  case RO:
    md.reqsRO.insert(dest);
    break;
  default:
    assert(0 && "Unexpected flag");
    abort();
  }
  outstandingReqs = 1;
}

void LocalDirectory::recvObjectImpl(fatPointer fptr) {
  Lockable* ptr = static_cast<Lockable*>(fptr.getObj());
  metadata* md = getMD(ptr);
  uint32_t self = NetworkInterface::ID;
  assert(ptr.getHost() == self);
  assert(md);
  assert(md->locRW != self && md->locRW != ~0);
  assert(md->locRO.empty());
  assert(hasAcquired(ptr));
  std::lock_guard<LL::SimpleLock> lg(md->lock, std::adopt_lock);
  md->locRW = ~0;
  md->recalledFor = ~0;
  dirRelease(ptr);
  outstandingReqs = 1; // reprocess outstanding reqs now that some may go forward
}

bool LocalDirectory::dirAcquire(Lockable* ptr) {
  std::lock_guard<LL::SimpleLock> lg(dirContext_lock);
  auto rv = dirContext.tryAcquire(ptr);
  switch (rv) {
  case LockManagerBase::FAIL:      return false;
  case LockManagerBase::NEW_OWNER: return true;
  case LockManagerBase::ALREADY_OWNER: assert(0 && "Already owner?"); abort();
  }
}

void LocalDirectory::dirRelease(Lockable* ptr) {
  std::lock_guard<LL::SimpleLock> lg(dirContext_lock);
  dirContext.releaseOne(ptr);
}

bool LocalDirectory::updateObjState(Lockable* ptr, metadata& md) {
  //fast exit
  if (md.reqsRO.empty() && md.reqsRW.empty())
    return true;

  //Is currently remote writer-only
  //recall for either next writer or first reader
  if (md.locRW != ~0) {
    uint32_t requester = ~0;
    if (!md.reqsRW.empty())
      requester = *md.reqsRW.begin();
    else 
      requester = *md.reqsRO.begin();
    //don't send duplicate recalls
    if (requester < md.recalledFor) {
      //send recall
      md.recalledFor = requester;
      md.t->invalidate(ptr, md.locRW, requester);
    }
    return true; //nothing left to do while still in remote Write
  }

  //Is currently remote read-only
  if (!md.locRO.empty()) {
    if (!md.reqsRW.empty()) {
      //invalidate remote reads and send obj to writer
      //bonus: check if this is an upgrade and ack that
      uint32_t requester = *md.reqsRW.begin();
      md.reqsRW.erase(md.reqsRW.begin());
      for (auto d : md.locRO) {
        if (d == requester) {
          //upgrade
          requester = ~0;
          md.t->upgrade(ptr, d);
        } else {
          //invalidate
          md.t->invalidate(ptr, d);
        }
      }
      md.locRO.clear();
      //Didn't upgrade, so send
      if (requester != ~0) {
        md.t->sendObj(ptr, requester, RW);
      }
    } else {
      assert(!md.reqsRO.empty());
      for (auto d : md.reqsRO)
        md.t->sendObj(ptr, d, RO);
      md.locRO.insert(md.reqsRO.begin(), md.reqsRO.end());
      md.reqsRO.clear();
    }
    return true; //nothing left to do
  }

  //object is currently local
  assert(md.locsRO.empty() && md.locRW == ~0);
  //object may be available, try to lock and send it
  if (dirAcquire(ptr)) {
    //favor writers
    if (!md.reqsRW.empty()) {
      //send object
      uint32_t dest = *md.reqsRW.begin();
      md.reqsRW.erase(md.reqsRW.begin());
      md.locRW = dest;
      md.t->sendObj(ptr, dest, RW);
    } else {
      //send to all readers
      for (auto d : md.reqsRO) {
        md.locRO.insert(d);
        md.t->sendObj(ptr, d, RO);
      }
      md.reqsRO.clear();
    }
    return true;
  } else {
    //Object is locked locally
    //revisit later
    return false;
  }
}

void LocalDirectory::makeProgress() {
  if (outstandingReqs) {
    outstandingReqs = 0; // clear flag before examining requests
    //inefficient, long held lock? (JUST A GUESS)
    std::lock_guard<LL::SimpleLock> lg(dir_lock);
    for (auto ii = dir.begin(), ee = dir.end(); ii != ee; ++ii) {
      std::lock_guard<LL::SimpleLock> obj_lg(ii->second.lock);
      if (!updateObjState(ii->first, ii->second))
        outstandingReqs = 1;
    }
  }
}

void LocalDirectory::dump() {
  //FIXME: write
}

////////////////////////////////////////////////////////////////////////////////

LocalDirectory& Galois::Runtime::getLocalDirectory() {
  static LocalDirectory obj;
  return obj;
}

RemoteDirectory& Galois::Runtime::getRemoteDirectory() {
  static RemoteDirectory obj;
  return obj;
}

CacheManager& Galois::Runtime::getCacheManager() {
  static CacheManager obj;
  return obj;
}

