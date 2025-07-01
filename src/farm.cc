// Copyright (c) 2018 The GAM Authors 

#include "farm.h"
#include "farm_txn.h"
#include "log.h"
#include "zmalloc.h"
#include "kernel.h"

#include <cstring>

using std::vector;
using std::unique_ptr;
using std::string;
using std::unordered_map;

#define vstring std::vector<std::string>

Farm::Farm(Worker* w): w_(w), tx_(nullptr), wh_(new WorkerHandle(w)), rtx_(new TxnContext()) {}
//构造函数，初始化Worker指针w_，事务指针tx_，WorkerHandle智能指针wh_和TxnContext智能指针rtx_
int Farm::txBegin() {
  if (unlikely(tx_ != nullptr)) { //检查当前事务指针tx_是否为空，如果不为空，表示已经有事务在运行。则无法开始新事务
    epicLog(LOG_INFO, "There is already a transaction running; You may want to call txCommit before start a new one!!!");
    return -1; //防止在已有事务的情况下再次开始新事务，确保事务的正确性和一致性。返回-1表示错误
  }
  //智能指针会在对象销毁时自动释放对象的内存，而普通指针不会管理对象的生命周期，需要手动释放内存。
  //将事务上下文对象与当前事务关联。使用智能指针管理事务上下文的生命周期，避免内存泄漏。
  tx_ = rtx_.get();//rtx_是一个智能指针，而tx_是一个原始指针，为了让tx_指向TxnContext对象，需要调用rtx_.get()获取TxnContext对象的原始指针
  tx_->reset(); //reset()方法通常会清空事务的读写集合、锁状态等信息，为新事务做准备。确保事务上下文处于干净状态，避免收到之前事务的影响。
  return 0;
}
//开始一个事务，如果已经有事务在运行，则记录日志并返回-1，否则重置事务上下文并返回0

GAddr Farm::txAlloc(size_t size, GAddr addr){
  if (unlikely(tx_ == nullptr)) {
    epicLog(LOG_FATAL, "Call txBegin first before any transactional allocation/read/write/free");
    return Gnullptr;
  }

  tx_->wr_->op = FARM_MALLOC;
  tx_->wr_->addr = addr;

  // each object is associated with version and size
  tx_->wr_->size = size + sizeof(version_t) + sizeof(osize_t);

  wh_->SendRequest(tx_->wr_);//发送内存分配请求

  if (tx_->wr_->status == SUCCESS) {
    addr = tx_->wr_->addr;

    // mark this object as to free so that it will get released provided
    // not get written
    Object* o = tx_->createWritableObject(addr);

    // initiate this object as zero-byte
    o->setVersion(0);
    o->setSize(0);
    //txWrite(addr, nullptr, 0);
    return tx_->wr_->addr;
  }
  else
    return Gnullptr;
}
//分配事务内存，如果事务未开始则记录致命错误日志并返回空地址，否则发送分配请求并返回分配的地址

void Farm::txFree(GAddr addr) {
  if (tx_ == nullptr) {
    epicLog(LOG_FATAL, "Call txBegin first before any transactional allocation/read/write/free");
    return;
  }

  // mark it as "to be freed" by setting size to -1
  txWrite(addr, nullptr, -1);
}
// 释放事务内存，如果事务未开始则记录致命错误日志，否则则将对象标记未待释放

/*在C++中，原子操作通常通过标准库<atomic>提供的原子类型和函数来实现。__atomic_load_n是GCC提供的一组内置函数之一，用于执行原子操作。
这些内置函数在GCC的文档中有详细描述。
__atomic_load_n函数是一个GCC内置函数，用于以原子方式加载一个值。定义如下：type __atomic_load_n (type *ptr, int memorder)
ptr：指向要加载的值的指针。memorder：内存顺序的约束，指定如何同步内存操作。常见的值包括。。。见下文
在程序中__atomic_load_n用于以原子的方式加载对象的版本号，即以原子方式加载local指向的内存位置的值，并将其存储在after变量中。
内存顺序约束为__ATOMIC_ACQUIRE，这意味值加载操作将获取内存顺序，确保在此加载操作之后的所有读写操作不会被重排序到此加载操作之前。
内存顺序约束：
__ATOMIC_RELAXED：不进行任何同步或顺序约束
__ATOMIC_CONSUME：数据依赖顺序一致性
__ATOMIC_ACQUIRE：获取操作，确保在此操作之后的所有读写操作不会被重排序到此操作之前
__ATOMIC_RELEASE：释放操作，确保在此操作之前的所有读写操作不会被重排序到此操作之后
__ATOMIC_ACQ_REL：获取和释放操作的组合
__ATOMIC_SEQ_CST：顺序一致性，确保所有原子操作按顺序执行*/
osize_t Farm::txRead(GAddr addr, char* buf, osize_t size) {
  if (unlikely(tx_ == nullptr)) {//首先检查当前是否有事务正在进行。如果没有事务，则记录致命错误日志并返回-1.
    epicLog(LOG_FATAL, "Call txBegin first before any transactional allocation/read/write/free");
    return -1;
  }

  Object* o = tx_->getReadableObject(addr);//尝试从上下文中获取可读对象。如果已经存在可读对象，则跳转到success标签。
  // check if there is already a readable copy
  if (o != nullptr) {
    goto success;
  }

  if (this->w_->IsLocal(addr)) {  //如果地址是本地的，则进行本地处理
    // process locally
    void* local = w_->ToLocal(addr); //将全局地址转换为本地地址

    // lock-free read  //进行无锁读取，确保读取的数据版本一致
    version_t before, after;
    o = tx_->createReadableObject(addr);//创建一个可读对象o
    //o->deserialize((const char*)local);
    after = __atomic_load_n((version_t*)local, __ATOMIC_ACQUIRE); //使用原子操作读取对象的版本号
    do {
      before = after; //将读取的版本号赋值给before
      while(is_version_wlocked(before)) //如果对象的版本号被写锁定，则重新加载
        before = __atomic_load_n((version_t*)local, __ATOMIC_ACQUIRE);
      runlock_version(&before);//解锁版本号的读锁 
      char* t = (char*)local + sizeof(before);  //指向size的地址
      osize_t s;   //读取数据并设置对象的版本和大小
      t += readInteger(t, s); //将内存区域中存储的size_读取到s中
      o->setVersion(before);
      o->setSize(s);
      o->readEmPlace(t, 0, s);
      after = __atomic_load_n((version_t*)local, __ATOMIC_ACQUIRE);//再次加载版本号after，如果版本号发生变化，则重新读取
    } while (is_version_diff(before, after));  //如果读取的版本号与当前版本号不一致，则重新读取


    if (o->getSize() == -1 || o->getVersion() == 0) { //如果对象的大小为-1或者版本为0，则表示对象未被写入，移除可读对象并跳转到fail标签
      // version == 0 means this object has not been written after being
      // allocated
      tx_->rmReadableObject(addr);
      goto fail;
    }
    goto success;
  }
  //如果地址不是本地的，则进行远程处理
  tx_->wr_->op = FARM_READ; //设置操作类型为FARM_READ，并设置地址
  tx_->wr_->addr = addr;

  if (wh_->SendRequest(tx_->wr_) != SUCCESS)  { //发送请求，如果请求失败则跳转到fail标签
    goto fail;
  }

  o = tx_->getReadableObject(addr);

success:
  if (o && buf && o->getSize() > 0 && size >= o->getSize()) {
    return o->writeTo(buf);
  }

fail:
  return 0;
}
//读取事务数据，如果事务未开始则记录致命错误日志并返回-1.否则读取数据并返回读取的大小

osize_t Farm::txPartialRead(GAddr addr, osize_t offset, char* buf, osize_t size) {
  if (unlikely(tx_ == nullptr)) {
    epicLog(LOG_FATAL, "Call txBegin first before any transactional allocation/read/write/free");
    return -1;
  }

  Object* o = tx_->getReadableObject(addr);

  if (o == nullptr) {
    // first read the whole object
    txRead(addr, nullptr, 0);
    o = tx_->getReadableObject(addr);
  }

  if (likely(o)) {
    return o->writeTo(buf, offset, size);
  }

  return 0;
}
//部份读取事务数据，如果事务未开始则记录致命错误日志并返回-1，否则读取部份数据并返回读取的大小

osize_t Farm::txPartialWrite(GAddr addr, osize_t offset, const char* buf, osize_t size) {
  if (unlikely(tx_ == nullptr)) {
    epicLog(LOG_FATAL, "Call txBegin first before any transactional allocation/read/write/free");
    return -1;
  }

  bool blind = true;
  if (tx_->getReadableObject(addr) || tx_->getWritableObject(addr))
    blind = false;

  if (blind) {
    txRead(addr, nullptr, 0);
    //tx_->rmReadableObject(addr);
  }

  Object* o = tx_->createWritableObject(addr);
  return o->readEmPlace(buf, offset, size);
}
//部份写入事务数据，如果事务未开始则记录致命错误日志并返回-1，否则写入部份数据并返回写入的大小

osize_t Farm::txWrite(GAddr addr, const char* buf, osize_t size) {
  if (unlikely(tx_ == nullptr)) { //检查当前是否有事务正在运行，如果没有事务，则记录知名错误日志并返回-1
    epicLog(LOG_FATAL, "Call txBegin first before any transactional allocation/read/write/free");
    return -1;
  }

  Object* o = tx_->createWritableObject(addr); //调用createWritableObject方法创建一个可写的对象Object
  if (size < 0 || o->getSize() == -1) {//检查写入内容的大小size是否小于0或对象的大小是否为-1，如果是，则将对象标记为待释放并返回0
    // free this object
    o->setSize(-1);
    return 0;
  }
  else
    o->setSize(o->readEmPlace(buf, 0, size)); //将数据写入对象并设置对象的大小，最后返回写入的大小。Object类的readEmPlace方法用于将数据写入对象
  return size;
}
//写入事务数据，如果事务未开始则记录致命错误日志并返回-1，否则写入数据并返回写入的大小

/*Farm::txCommit函数用于提交当前事务。它首先检查是否有事务正在运行，如果没有则记录致命错误日志并返回-1，然后根据事务是否为本地事务分别处理。
对于本地事务，调用FarmProcessLocalCommit方法处理提交，并根据提交结果返回0/-1
对于远程事务，调用SendRequest方法发送提交请求，并根据请求结果返回0/-1.
在提交完成之后，无论成功与否，都会将事务指针tx_设置为空，以表示当前没有活跃的事务。*/
int Farm::txCommit() {
  if (unlikely(tx_ == nullptr)) {//检查事务是否正在进行，如果没有事务，则记录致命错误日志并返回-1
    epicLog(LOG_FATAL, "There is no active transaction!!!!!!!");
    return -1;
  }

  if (txnIsLocal()){//检查事务是否是本地事务
    tx_->wr_->op = Work::FARM_READ; // a trick to indicate this is an app commit 设置操作类型为Worker::FARM_READ，这是一个技巧，用于标记当前事务是由应用程序线程发起的本地提交，在后续的FarmProcessLocalCommit函数中，系统会根据操作类型为FARM_READ的请求执行本地事务提交逻辑
    this->w_->FarmProcessLocalCommit(tx_->wr_);//调用FarmProcessLocalCommit方法处理本地提交，该函数会检查事务的写集合、锁状态等，并决定提交或回滚事务
    bool ret = (tx_->wr_->status == Status::SUCCESS) ? 0 : -1;  //根据提交结果设置返回值
    tx_ = nullptr;//清理事务上下文，将事务指针tx_设置为空，标识当前没有活跃的事务。
    return ret;
  }

  //tx_->updateVesion();
  tx_->wr_->op = COMMIT;//如果事务不是本地事务，则设置操作类型为COMMIT，标记当前事务为分布式事务提交
  int ret = wh_->SendRequest(tx_->wr_); //调用SendRequest方法发送提交请求，将提交请求发送到协调器或其他节点。该请求会触发分布式事务的两阶段提交协议。
  tx_ = nullptr;  //清理事务上下文，将事务指针tx_设置为空
  return ret != 0 ? -1 : ret; //根据提交结果设置返回值
}
//提交事务，如果事务未开始则记录致命错误日志并返回-1，否则提交事务并返回结果

int Farm::txAbort() {
  if (unlikely(tx_ == nullptr)) {
    epicLog(LOG_FATAL, "There is no active transaction!!!!!!!");
    return -1;
  }

  tx_->reset();
  tx_ = nullptr;
  return 0;
}
//中止事务，如果事务未开始则记录致命错误日志并返回-1，否则重置事务上下文并返回0

int Farm::put(uint64_t key, const void* value, size_t count) {
  this->txBegin();
  WorkRequest* wr = this->tx_->wr_;
  wr->size = count;
  wr->key = key;
  wr->ptr = const_cast<void*>(value);
  wr->op = PUT;

  int ret = -1;

  if (this->wh_->SendRequest(wr)) {
    epicLog(LOG_WARNING, "Put failed");
  } else {
    ret = count;
  }

  this->txCommit();
  return ret;
}
//存储键值对，开始事务，发送存储请求，提交事务并返回结果

int Farm::get(uint64_t key, void* value) {
  this->txBegin();
  WorkRequest* wr = this->tx_->wr_;
  wr->key = key;
  wr->op = GET;
  wr->ptr = value;
  int ret = -1;

  if (wh_->SendRequest(wr)) {
    epicLog(LOG_WARNING, "Get failed");
  } else {
    ret = wr->size;
    //memcpy(value, wr->ptr, wr->size);
  }

  this->txCommit();
  return ret;
}
//获取键值对，开始事务，发送获取请求，提交事务并返回结果

int Farm::kv_put(uint64_t key, const void* value, size_t count, int node_id) {
  bool newtx = false;
  if(likely(tx_ == nullptr)) newtx = true;
  if(newtx) this->txBegin();
  WorkRequest* wr = this->tx_->wr_;
  wr->size = count;
  wr->key = key;
  wr->ptr = const_cast<void*>(value);
  wr->op = KV_PUT;
  wr->counter = node_id;

  int ret = -1;

  if (this->wh_->SendRequest(wr)) {
    epicLog(LOG_INFO, "Put failed");
  } else {
    ret = count;
  }

  if(newtx) this->txCommit();
  return ret;
}
//存储键值对到指定节点，如果事务未开始则开始事务，发送存储请求，提交事务并返回结果

int Farm::kv_get(uint64_t key, void* value, int node_id) {
  bool newtx = false;
  if(likely(tx_ == nullptr)) newtx = true;
  if(newtx) this->txBegin();
  WorkRequest* wr = this->tx_->wr_;
  wr->key = key;
  wr->op = KV_GET;
  wr->ptr = value;
  wr->counter = node_id;
  int ret = -1;

  if (wh_->SendRequest(wr)) {
    epicLog(LOG_INFO, "Get failed");
  } else {
    ret = wr->size;
    //memcpy(value, wr->ptr, wr->size);
  }

  if(newtx) this->txCommit();
  return ret;
}
//从指定节点获取键值，如果事务未开始则开始事务，发送获取请求，提交事务并返回结果
