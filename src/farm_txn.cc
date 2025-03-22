// Copyright (c) 2018 The GAM Authors 

#include "structure.h"
#include "farm_txn.h"
#include "chars.h"
#include "log.h"

#include <cstring>

Object::Object(std::string& s, GAddr addr): buf_(s), addr_(addr), pos_(-1) {}

/**
 * @brief write @param size bytes from position @param offset of this object into @param obuf
 *
 * @param obuf
 * @param offset
 * @param size
 *
 * @return number of objects written
 */
osize_t Object::writeTo(char* obuf, int offset, osize_t size) {
  epicAssert(pos_ >= 0);

  if (size == -1) {
    // read all reaming bytes
    size = size_ - offset;
  }

  if (offset + size > size_ || size <= 0 || offset < 0 || size_ == -1)
    // invalid param
    return 0;

  memcpy(obuf, buf_.c_str() + pos_ + offset, size);
  return size;
}

/**
 * @brief replace the object with the content of @param ibuf
 *
 * @param ibuf
 *
 * @return number of bytes within this object
 */
osize_t Object::readFrom(const char* ibuf) {
  pos_ = buf_.size();
  if (this->size_ <= 0)
    return 0;

  buf_.append(ibuf, this->size_);
  return this->size_;
}

/**
 * @brief read @param size bytes from @param ibuf into position @offset of this
 * object
 *
 * @param ibuf
 * @param offset    the position from which this object shall be written 
 * @param size
 *
 * @return  the nubmer of written bytes
 */
osize_t Object::readEmPlace(const char* ibuf, osize_t offset, osize_t size) {
  if (offset < 0 || offset > size_ || size < 0)//首先检查offset和size是否有效，如果无效，则返回0
    return 0;

  epicAssert(this->size_ >= 0); //确保对象的大小非负

  if (this->pos_ == -1) //如果pos_为-1，表示对象还没有数据
    this->pos_ = buf_.length();//设置pos_为数据缓冲区的当前长度

  if (offset + size > this->size_) { //如果offset+size超过当前对象大小size_，则扩展缓冲区并追加数据
    buf_.append(buf_, pos_, offset).append(ibuf, size); //将buf_中从pos_开始的offset个字符追加到buf_的末尾；将ibuf中的size个字符追加到buf_的末尾
    this->size_ = offset + size;
    this->pos_ = buf_.length() - size_;
  } else {//否则，替换缓冲区中指定位置的数据
    buf_.replace(this->pos_ + offset, size, ibuf, size); //将buf_中从pos_+offset开始的size个字符替换为ibuf中的size个字符
  } 

  return size; //返回写入的大小
}

osize_t Object::serialize(char* dst, osize_t len) {
  if (len < this->getTotalSize())
    return 0;

  char *buf = dst;
  buf += appendInteger((char*)buf, this->version_, this->size_);
  return buf - dst + writeTo(buf, 0, size_);
}

osize_t Object::deserialize(const char* msg, osize_t size) {
  char* buf = (char*)msg;

  int mdsize = sizeof(version_t) + sizeof(osize_t);

  if ((size < 0 && size != -1) || (size >= 0 && size < mdsize))
    return 0;

  // note the addr_ shall not be included in msg
  buf += readInteger(buf, this->version_, this->size_);
  if (size < this->size_ + mdsize && size > 0 )
    return 0;

  epicAssert(!is_version_locked(this->version_));
  //runlock_version(&this->version_);

  readEmPlace(buf, 0, this->size_);
  return this->getTotalSize();
}

const char* Object::toString() {
  static char s[100];
  memset(s, 0, 100);
  sprintf(s, "addr = %lx, version = %lx, size = %d, locked = %d", 
      addr_, version_, size_, is_version_locked(version_)?1:0);
  return s;
}

int TxnContext::generatePrepareMsg(uint16_t wid, char* buf, int len, int& nobj) {
  int pos = 0, cnt = 0;

  for (auto& p : this->write_set_.at(wid)) {
    if (cnt++ < nobj) continue;
    Object* o = p.second.get();
    if (pos + o->getSize() + sizeof(osize_t) + sizeof(GAddr) > len)
      break;
    pos += appendInteger(buf+pos, o->getAddr(), o->getSize());
    //pos += o->serialize(buf+pos);
    pos += o->writeTo(buf+pos);
    nobj++;
  }

  return pos;
}

int TxnContext::generateValidateMsg(uint16_t wid, char* buf, int len, int& nobj ) {
  int pos = 0, cnt = 0;

  for (auto& p: this->read_set_.at(wid)) {
    // skip writable objects
    // if (p.second.use_count() > 1) continue;

    if (cnt++ < nobj) continue;

    Object* o = p.second.get();

    epicAssert(o->getVersion() > 0);

    if (pos + sizeof(version_t) + sizeof(GAddr) > len)
      break;

    pos += appendInteger(buf+pos, o->getAddr(), o->getVersion());
    nobj ++;
  }

  return pos;
}

int TxnContext::generateCommitMsg(uint16_t wid, char* buf, int len) {
  /* this should never be called */
  return 0;
}
/*获取可读对象：首先检查读集合中是否存在给定地址的对象，如果存在则返回该对象。
否则检查写集合中是否存在该对象，如果存在则返回该对象*/
Object* TxnContext::getReadableObject(GAddr addr) {
  //if (read_set_[WID(addr)].count(addr) == 0) {
  //    //createReadableObject(addr);
  //    return nullptr;
  //}

  Object* o = nullptr;

  if (read_set_[WID(addr)].count(addr) > 0)
    o = read_set_[WID(addr)][addr].get();
  else if (write_set_[WID(addr)].count(addr) > 0)
    o = write_set_[WID(addr)][addr].get();

  return o;
}

Object* TxnContext::createReadableObject(GAddr addr) {
  epicAssert(read_set_[WID(addr)].count(addr) == 0 && write_set_[WID(addr)].count(addr) == 0 && WID(addr) > 0);

  epicLog(LOG_DEBUG, "Txn %d creates a readable object for address %lx", this->wr_->id, addr);

  this->read_set_[WID(addr)][addr] =
    std::shared_ptr<Object>(new Object(this->buffer_, addr));

  return read_set_[WID(addr)][addr].get();
}

Object* TxnContext::getWritableObject(GAddr addr) {
  //if (write_set_[WID(addr)].count(addr) == 0)
  //    return nullptr;

  return write_set_[WID(addr)].count(addr) > 0 ? write_set_[WID(addr)][addr].get() : nullptr;
}
/*createWritableObject函数是TxnContext类的一部分，用于在事务上下文中创建一个可写的对象，
该函数检查给定地址的对象是否已经存在于写集合中，如果不存在，则根据读集合中的对象或创建一个新的对象，
将其添加到写集合中。
通过在写集合中创建可写对象，确保事务的隔离性，每个事务在自己的上下文中操作对象，不会直接影响其他事务。
只有在需要写操作时才创建对象，避免不必要的内存分配，提高性能。*/
Object* TxnContext::createWritableObject(GAddr addr) {//GAddr addr——全局地址，用于标识对象的位置
  epicAssert(WID(addr) > 0); //使用断言检查地址的有效性，确保WID(addr)大于0
  if (write_set_[WID(addr)].count(addr) == 0) { //检查写集合中是否已经存在给定地址的对象。如果不存在，则继续执行创建过程。
    if ( read_set_[WID(addr)].count(addr) > 0) { //如果读集合中存在给定地址的对象，则共享该对象的所有权，将其添加到写集合中，避免重复创建对象，提高内存利用率
      /* share the ownership of object */
      this->write_set_[WID(addr)][addr] = this->read_set_[WID(addr)][addr];
    } else {  //如果读集合中不存在给定地址的对象，则创建一个新的对象，并将其添加到写集合中
      this->write_set_[WID(addr)][addr] =
        std::shared_ptr<Object>(new Object(this->buffer_, addr));
    }
    //记录日志，指示事务创建了一个可写的对象
    epicLog(LOG_DEBUG, "Txn %d creates a writable object for address %lx", this->wr_->id, addr);
  }
  return write_set_[WID(addr)][addr].get(); //返回写集合中给定地址的对象指针
}

void TxnContext::getWidForRobj(std::vector<uint16_t>& wid) {
  for (auto& p: this->read_set_) {
    if (getNumRobjForWid(p.first) > 0)
      wid.push_back(p.first);
  }
}

void TxnContext::getWidForWobj(std::vector<uint16_t>& wid) {
  for (auto& p: this->write_set_) {
    if (getNumWobjForWid(p.first) > 0)
      wid.push_back(p.first);
  }
}
/*重置事务上下文：清空读集合和写集合，清空缓冲区，并将工作请求的事务指针设置为当前事务上下文*/
void TxnContext::reset() {
  for (auto& p : this->read_set_) {
    p.second.clear();
  }

  for (auto& p : this->write_set_) {
    p.second.clear();
  }

  this->write_set_.clear();
  this->read_set_.clear();
  this->buffer_.clear();
  this->wr_->tx = this;
}
