#include <iostream>
#include <memory>
#include <mutex>
constexpr size_t MEMPOOL_ALIGNMENT = 8; // 对齐长度

using namespace std;

template <typename T>
struct memoryblock {
    int nsize;  //该内存块的大小
    int nfree;  //该内存块空闲内存单元个数
    int nfirst; //当前可用的第一个内存单元的序号
    unique_ptr<memoryblock<T>> pnext;   //指向下一个内存块
    char adata[1];  //标记该内存块"管理区域"的起始位置

    memoryblock(int nunitsize, int nunitamount)
        : nsize(nunitsize * nunitamount), nfree(nunitamount - 1), nfirst(1), pnext(nullptr) {
        char* ppdata = adata;
        //在初始内存块时，内存块 的 每个内存单元 都是空闲的，每个 内存单元 记录 下一个空闲内存单元的索引
        //由于 空闲单元 从 1 开始，所以 第一个空闲单元 的地址不是 adata，而是 adata + nunitsize
        //adata 指向的第 0 个单元仅仅用作空闲链表的管理数据，通常不作为可分配单元使用
        for (int i = 1; i < nunitamount; ++i) {
            (*(unsigned short*)ppdata) = i;
            ppdata += nunitsize;
        }
        cout << "MemoryBlock created. nsize: " << nsize << ", nfree: " << nfree << endl;
    }

    void* operator new(size_t, int nunitsize, int nunitamount) {
        return ::operator new(sizeof(memoryblock) + nunitsize * nunitamount);
    }

    void operator delete(void* pblock) {
        ::operator delete(pblock);
    }
};

template <typename T>
struct memorypool {
    int ninitsize;  // 首块 内存单元的个数
    int ngrowsize;  // 后续块 内存单元的个数
    int nunitsize;  // 内存单元的大小 
    unique_ptr<memoryblock<T>> pblock;  //指向 内存池 中首个 内存块
    mutex mtx;  //用于多线程安全

    memorypool(int _ngrowsize = 10, int _ninitsize = 3)
        : ninitsize(_ninitsize), ngrowsize(_ngrowsize), pblock(nullptr) {
        nunitsize = align_size(sizeof(T));
    }

    ~memorypool() {

    }

    //将 nunitsize 按照 MEMPOOL_ALIGNMENT = 8 字节对齐，确实是为了确保内存单元的起始地址是 8 的倍数，便于cpu进行访问
    //如果 size 本身已经是对齐的，它将保持不变
    //如果不是，则会将它“向上”调整为下一个对齐边界
    static size_t align_size(size_t size) {
        return (size + (MEMPOOL_ALIGNMENT - 1)) & ~(MEMPOOL_ALIGNMENT - 1);
    }

    //向 内存池 申请 num 个内存单元，返回可用 内存单元 的地址
    void* allocate(size_t num) {
        lock_guard<mutex> lock(mtx); // 确保线程安全
        for (size_t i = 0; i < num; ++i) {
            //如果 内存池 中没有 内存块，则创建 首块，返回 首块 的"管理区域"的开始地址
            if (!pblock) {
                pblock = unique_ptr<memoryblock<T>> (new(nunitsize, ninitsize) memoryblock<T>(nunitsize, ninitsize));
                cout << "Allocated new memory block, returning address: " << (void*)pblock->adata << endl;
                return pblock->adata;
            }

            //如果 内存池 中有 内存块，则查找是否存在 空闲的内存块
            auto* pmyblock = pblock.get();
            while(pmyblock && pmyblock->nfree == 0){
                pmyblock = pmyblock->pnext.get();
            }

            //如果存在 空闲的内存块
            //1.根据 该内存块的 nfirst 和 adata 计算出 第一个空闲单元的地址 pfree ，将其作为返回值
            //2.更新 该内存块的 nfirst 为 pfree 保存的 下一个空闲单元的序号
            //3.更新 该内存块的 nfree--
            if (pmyblock) {
                char* pfree = pmyblock->adata + pmyblock->nfirst * nunitsize;
                pmyblock->nfirst = *((unsigned short*)pfree);
                --pmyblock->nfree;
                cout << "Allocated memory unit from block, remaining free units: " << pmyblock->nfree << endl;
                return pfree;
            } 
            
            //如果不存在 空闲的空闲块
            //1.创建一个新的内存块 new_block
            //2.将 new_block 的 pnext 指向当前 内存池 中的第一个内存块 pblock
            //3.更新当前 内存池 的第一个内存块 pblock 为 new_block
            //4.将 new_block 的 adata 地址作为返回值
            else {
                auto new_block = unique_ptr<memoryblock<T>> (new(nunitsize, ngrowsize) memoryblock<T>(nunitsize, ngrowsize));
                new_block->pnext = move(pblock);
                pblock = move(new_block);
                cout << "Allocated new memory block, returning address: " << (void*)pblock->adata << endl;
                return pblock->adata;
            }
        }
        return nullptr;
    }

    //向 内存池 释放 pfree 指向的内存单元：每次释放时，都将被释放的单元插入内存块的空闲单元链表的头部，从而更新 nfirst
    void free(void* pfree) {
        lock_guard<mutex> lock(mtx); // 确保线程安全
        auto* pmyblock = pblock.get();
        memoryblock<T>* preblock = nullptr;

        //查找 pfree 所处的内存块 
        while(pmyblock){
            if(pfree >= pmyblock->adata && pfree < pmyblock->adata + pmyblock->nsize){
                break;
            }
            preblock = pmyblock;
            pmyblock = pmyblock->pnext.get();
        }

        //如果 pfree存在于内存池中的某个内存块中
        //1.让 pfree 指向的空间保存 该内存块 之前的第一个空闲单元序号
        //2.更新 该内存块 的第一个空闲单元序号为 pfree 所指向的序号
        //3.更新 该内存块的 nfree++
        if (pmyblock) {
            *((unsigned short*)pfree) = pmyblock->nfirst;
            pmyblock->nfirst = (reinterpret_cast<char*>(pfree) - pmyblock->adata) / nunitsize;
            ++pmyblock->nfree;
            cout << "Freed memory unit, new first free unit: " << pmyblock->nfirst << ", remaining free units: " << pmyblock->nfree << endl;

            // 如果整个块空闲，释放整个块
            if (pmyblock->nfree * nunitsize == pmyblock->nsize) {
                if (preblock) {
                    preblock->pnext = move(pmyblock->pnext);
                } else {
                    pblock = move(pmyblock->pnext);
                }
                cout << "Memory block completely freed and removed." << endl;
            }
        }
    }
};

// 测试类
class user {
    int s;

public:
    user(int x) : s(x) {}
    int get() const { return s; }
    ~user() {}
};

int main() {
    memorypool<user> m_pool;
    
    // 分配 1 个内存单元
    user* dp1 = (user*)m_pool.allocate(1);
    if (dp1) {
        new (dp1) user(1111);
        cout << "Object 1 data: " << dp1->get() << endl;
    }

    // 再次分配 1 个内存单元
    user* dp2 = (user*)m_pool.allocate(1);
    if (dp2) {
        new (dp2) user(2222);
        cout << "Object 2 data: " << dp2->get() << endl;
    }

    // 释放第一个对象的内存
    m_pool.free(dp1);

    // 再次分配 1 个内存单元
    user* dp3 = (user*)m_pool.allocate(1);
    if (dp3) {
        new (dp3) user(3333);
        cout << "Object 3 data: " << dp3->get() << endl;
    }

    // 释放第二个和第三个对象的内存
    m_pool.free(dp2);
    m_pool.free(dp3);

    return 0;
}

