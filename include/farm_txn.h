// Copyright (c) 2018 The GAM Authors 

#ifndef FARM_TXN_H
#define FARM_TXN_H
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include "structure.h"
#include "workrequest.h"
#include "chars.h"

typedef int32_t osize_t;

/*在这个系统中，版本号只占用了56位，这是为了在64位的version_t类型中保留高8位用于存储锁信息。
这样设计的目的是为了在同一个变量中同时管理版本号和锁状态，从而简化并发控制和版本管理。
版本号：占用低56位，用于存储实际的版本号。
锁信息：占用高8位，用于存储锁状态(读锁和写锁)*/
typedef uint64_t version_t;
#define VBITS 56  //定义了版本号中用于存储实际版本号的位数
#define MAX_VERSION ((1UL << VBITS) - 1) //用于表示版本号的最大值
#define UNLOCK 0x0 
#define RLOCK 0x1 
#define WLOCK 0x2
#define RWLOCK (RLOCK | WLOCK)
//将版本号的写锁位清零
static inline void wunlock_version(version_t* v) {
    version_t v1 = RLOCK;
    (*v) &= ((v1 << VBITS) | MAX_VERSION);
}
//将版本号的读锁位清零
static inline void runlock_version(version_t* v) {
    version_t v1 = WLOCK;
    (*v) &= ((v1 << VBITS) | MAX_VERSION);
}
//将版本号的所有锁位清零 
static inline void unlock_version(version_t* v) {
    *v &= MAX_VERSION;
}
//将版本号的读锁位设置为1
static inline void rlock_version(version_t* v) {
    version_t v1 = RLOCK;
    *v |= (v1 << VBITS);
}
//将版本号的写锁位设置为1
static inline void wlock_version(version_t* v) {
    version_t v1 = WLOCK;
    *v |= (v1 << VBITS);
}
//检查版本号的读锁位是否被设置
static inline bool is_version_rlocked(version_t v) {
    return (v >> VBITS) & RLOCK;
}
/*检查给定的版本号是否被写锁定
参数——v：一个version_t类型的变量，表示对象的版本号
宏定义——VBITS   56：定义了版本号中用于存储实际版本号的位数。
       WLOCK  0x2：定义了写锁的标志位.
具体来说，该函数将版本号v右移VBITS位，然后与WLOCK进行按位与操作。如果结果不为0，则表示版本号被写锁定。*/
static inline bool is_version_wlocked(version_t v) {
    return (v >> VBITS) & WLOCK;
}
//检查版本号的读锁位或者写锁位是否被设置
static inline bool is_version_locked(version_t v) {
    return is_version_rlocked(v) | is_version_wlocked(v);
}
//该函数用于检查两个版本号是否不同。如果版本号不同，或者其中一个版本号被写锁定，则返回true，否则返回false。 
static inline bool is_version_diff(version_t before, version_t after) {
    if (is_version_wlocked(before) || is_version_wlocked(after)) 
        return true; //首先检查before和after是否被写锁定，如果有一个被写锁定，则返回true 

    runlock_version(&before); //将before和after的读锁位清零
    runlock_version(&after);
    epicAssert(!is_version_locked(before) && !is_version_locked(after)); //使用断言减产before和after是否都没有被锁定。如果断言失败，程序会终止
    if (before != after) //比较before和after两个版本号，如果不相等，则返回true
        return true;
    return false;//如果上述检查都通过，则返回false，表示版本号相同
}

static inline bool rlock_object(void* object) {
    version_t ov, nv;
    ov = __atomic_load_n((version_t*)object,  __ATOMIC_RELAXED);
    if (is_version_locked(ov))
        // rlock an object only if it is free
        return false;
    nv = ov;
    rlock_version(&nv);
    return __atomic_compare_exchange_n((version_t*)object, &ov, nv, true,
            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static inline bool wlock_object(void *object) {
    version_t ov, nv;
    ov = __atomic_load_n((version_t*)object, __ATOMIC_RELAXED);
    epicAssert(is_version_rlocked(ov) && !is_version_wlocked(ov));
    nv = ov;
    runlock_version(&nv);
    if (++nv == MAX_VERSION) {
        nv == 1;
    }
    wlock_version(&nv);
    return __atomic_compare_exchange_n((version_t*)object, &ov, nv, true,
            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static inline void runlock_object(void* object) {
    version_t ov;
    ov = __atomic_load_n((version_t*)object, __ATOMIC_RELAXED);
    epicAssert(is_version_rlocked(ov) && !is_version_wlocked(ov));
    runlock_version(&ov);
    __atomic_store_n((version_t*)object, ov, __ATOMIC_RELAXED);
}

static inline void wunlock_object(void* object) {
    version_t ov;
    ov = __atomic_load_n((version_t*)object, __ATOMIC_RELAXED);
    epicAssert(is_version_wlocked(ov) && !is_version_rlocked(ov));
    wunlock_version(&ov);
    __atomic_store_n((version_t*)object, ov, __ATOMIC_RELEASE);
}

static inline bool is_object_rlocked(void* object) {
    version_t v = __atomic_load_n((version_t*)object, __ATOMIC_RELAXED);
    return is_version_rlocked(v);
}

static inline bool is_object_wlocked(void* object) {
    version_t v = __atomic_load_n((version_t*)object, __ATOMIC_ACQUIRE);
    return is_version_wlocked(v);
}

static inline bool is_object_locked(void* object) {
    version_t v = __atomic_load_n((version_t*)object, __ATOMIC_RELAXED);
    return is_version_locked(v);
}


//#define PREPARE 1
//#define VALIDATE 2
//#define ABORT 3
//#define COMMIT 4

class TxnContext;
/*Object类的设计目的是表示事务中的对象，包含了对象的地址、版本、大小和数据缓冲区等信息。
它提供了序列化和反序列化的方法，以及数据读写方法。主要用途：
1.对象管理：addr_，version_，size_，buf_用于存储对象的基本信息和数据。
2.序列化和反序列化：serialize，deserialize方法用于将对象转化为字符数组和从字符数据转换为对象，便于在网络上传输或持久化存储。
3.数据读取：writeTo和readFrom方法用于将数据写入制定缓冲区和从指定缓冲区读取对象数据。readEmPlace用于就地读取数据。
4.版本管理：setVersion和getVersion方法同于设置和获取对象的版本。
5.大小管理：getSize，setSize方法用于获取和设置对象的数据大小。getTotalSize方法用于获取对象的总大小，包括版本和大小字段。
6.内容管理：hasContent方法用于检查对象是否有内容。freeContent方法用于释放对象内容*/
class Object {
    /* object layout: |version|size|data| */
    private:
        GAddr addr_;    //对象的地址
        version_t version_;//对象的版本
        uint32_t pos_;//数据在缓冲区中的位置
        osize_t size_;//对象的数据大小
        std::string& buf_;//数据缓冲区的引用

    public:
        Object(std::string&, GAddr); //构造函数，初始化对象的地址和数据缓冲区

        osize_t deserialize(const char*, osize_t = -1);//反序列化，将字符数据转换为对象
        osize_t serialize(char*, osize_t);//序列化，将对象转换为字符数据
        osize_t writeTo(char*, int = 0, osize_t = -1);//写入数据，将对象数据写入指定缓冲区
        osize_t readFrom(const char*);//读取数据，从指定缓冲区读取对象数据
        osize_t readEmPlace(const char*, osize_t, osize_t);//就地读取数据，

        inline GAddr getAddr() {return addr_;}//获取对象地址

        inline void setVersion(version_t v) {this->version_ = v;}//设置对象版本
        inline version_t getVersion() {return version_;}//获取对象版本

        inline osize_t getSize() {return size_;}//获取对象大小
        inline void setSize(osize_t sz) { size_ = sz; }//设置对象大小

        inline osize_t getTotalSize() {//获取对象的总大小
            if (size_ == -1)
                return sizeof(version_t) + sizeof(osize_t);
            else
                return size_ + sizeof(version_t) + sizeof(osize_t);
        }

        inline bool hasContent() {return (pos_ != -1);}//检查对象是否有内容
        inline void freeContent() { pos_ = -1; }//释放对象内容

        const char* toString(); //将对象转换为字符串表示
        //inline void unlock() {unlock_version(&this->version_);}
        //inline void lock() {lock_version(&this->version_);}
        //inline bool isLocked() {return is_version_locked(this->version_);}
};
/*TxnContext类的设计目的是管理事务上下文，包含了事务操作所需的各种信息和方法。
它提供了对读写集合的管理、事务消息的生成、事务对象的创建和获取等功能。
1.读写结合管理：write_set_和read_set_分别用于存储事务的写集合和读集合，按工作节点ID分组。
2.事务消息生成：提供了生成准备消息、验证消息、提交消息和中止消息的方法，用于事务的不同阶段。
3.事务对象管理：提供了创建和获取可读对象和可写对象的方法。
4.同步和重置：提供了重置事务上下文的方法，用于事务的重新开始。*/
class TxnContext{
    private:
        /*std::unordered_map是C++标准库中的一种关联容器，提供了基于哈希表实现的键值对存储。每个元素由一个key和一个value组成，key用于唯一标识元素，value是与可以相关联的数据。
        std::unordered_map使用哈希表存储数据，因此查找、插入、删除操作的平均时间复杂度为O(1)。元素的存储顺序不固定，取决于哈希函数的结果。*/
        //写集合，按工作节点ID(uint16_t)分组,存储每个地址(GAddr)对应的对象。
        std::unordered_map<uint16_t, std::unordered_map<GAddr, std::shared_ptr<Object>>> write_set_;//<工作节点id，该工作节点的读集合<全局地址，与地址关联的对象>>，使用智能指针管理对象的生命周期
        //读集合，按工作节点ID(uint16_t)分组,存储每个地址(GAddr)对应的对象。
        std::unordered_map<uint16_t, std::unordered_map<GAddr, std::shared_ptr<Object>>> read_set_;
        //用于存储事务相关的缓冲区
        std::string buffer_; 


    public:
        WorkRequest* wr_; //指向工作请求的指针

        TxnContext(){wr_ = new WorkRequest;} //构造函数，初始化wr_为新的WorkRequest对象
        ~TxnContext() {delete wr_;}//析构函数，删除wr_对象。

        inline std::string& getBuffer() {return this->buffer_;} //返回缓冲区buffer_的引用

        Object* getReadableObject(GAddr); // {return read_set_.at(a>>48).at(a).get();}//获取可读对象
        Object* createNewWritableObject(GAddr);//创建新的可写对象
        Object* createReadableObject(GAddr);//创建可读对象
        Object* createWritableObject(GAddr);//创建可写对象
        Object* getWritableObject(GAddr);//获取可写对象
        inline bool containWritable(GAddr a) { //检查写集合中是否包含指定地址的对象
            return this->write_set_[WID(a)].count(a) > 0;
        }

        inline void rmReadableObject(GAddr a) {//从读集合中移除指定地址的对象
            this->read_set_[WID(a)].erase(a);
        }

        int generatePrepareMsg(uint16_t wid, char* msg, int len, int& nobj );//生成准备消息
        int generateValidateMsg(uint16_t wid, char* msg, int len, int& nobj ); //生成验证消息
        int generateCommitMsg(uint16_t wid, char* msg, int len);//生成提交消息
        int generateAbortMsg(uint16_t wid, char* msg, int len);//生成中止消息

        void getWidForRobj(std::vector<uint16_t>& wid);//获取读对象的工作节点ID
        void getWidForWobj(std::vector<uint16_t>& wid);//获取写对象的工作节点ID

        inline int getNumWobjForWid(uint16_t w) { //获取指定工作节点ID的写对象数量
            //return write_set_.count(w) > 0 ? write_set_.at(w).size() : 0;
            return write_set_.count(w) > 0 ? write_set_.at(w).size() : 0;
        }

        inline int getNumRobjForWid(uint16_t w) {//获取指定工作节点ID的读对象数量
            return read_set_.count(w) > 0 ? read_set_.at(w).size(): 0;
        }
        
        //获取指定工作节点ID的读集合
        inline std::unordered_map<GAddr, std::shared_ptr<Object>>& getReadSet(uint16_t wid) {
            epicAssert(read_set_.count(wid) == 1);
            return read_set_.at(wid);
        }

        //获取指定工作节点ID的写集合
        inline std::unordered_map<GAddr, std::shared_ptr<Object>>& getWriteSet(uint16_t wid) {
            epicAssert(write_set_.count(wid) == 1);
            return write_set_.at(wid);
        }

        void reset();//重置事务上下文

};

#endif
