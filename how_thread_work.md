## FlexiCAS multi-thread 

本文档阐述了FlexiCAS在开启多线程时是采用什么的方式来保持Cache Coherence和避免线程间死锁的，阅读它的前置知识为假设你已经了解了单线程的Cache是如何工作的(如probe、acquire以及writeback的基础工作流程)

## Overview

- 使用的多线程库为c++自带的std::thread，它可以灵活的控制线程间的挂起与运行
- 没有采用Gem5中线程间通信的方式完成同步，而是每一个线程负责完整的一次transaction(read/write/flush)，线程与线程间通过锁和信号量完成同步
- 目前只支持inclusive Cache

## 缓存组锁

- 在多级Cache中，多个线程想要操作同一个缓存块是完全有可能的（如两个不同的线程在同一时刻都在LLC上acquire地址X），为了防止出现多个线程同时写同一个缓存块的情况，我们需要让一个线程在操作缓存块的同时，另一个线程被挂起（如果真的出现了同时写的情况），在当前的线程完成操作后再通知其他线程结束挂起
- 在实际的实现时，我们让多个线程不能同时操作同一个缓存组（同一个set），如果两个线程都想acquire同一个set T（即使它们不想acquire同一个地址X），那么首先获得set T访问权的线程先运行，而另一个线程需要被挂起

## 优先级

- 只设定缓存组锁并不能满足多线程运行的需求，我们还需要规定每种请求(acquire/release/probe)的优先级才能保证不会出现死锁，试考虑不规定优先级的情况：
  
  **Case1**
  
  - 一个二级的Cache系统，两个L1（分别为L1 A和 L1B，以下将它们对应的线程简称A和B)，以及一个L2
  - T0时刻，A和B分别访问地址X和Y，由于在L1都miss，A和B都向L2发出acquire，由于地址X和Y对应的是同一个set T，两个线程出现竞争，A先拿到T的访问权，由于T是满的，需要踢出缓存块Z，A这时需要向L1  B发出probe请求。如果不规定请求的优先级并且Z在L1 B的对应的set是地址Y对应的set，那么A会在L1 B中等待L1 B acquire的结束，而B正在L2中等待A acquire的结束，即出现了A和B循环等待的情况，系统死锁
  
- 我们规定各个请求的优先级如下：release > probe > acquire。规定优先级后，即使两个请求在同一个缓存组发生了冲突，如果A请求的优先级比B请求的优先级高，那么即使B先拿到了缓存组锁，A也会抢占运行。

  在规定了优先级后重新考虑 **Case1**:

     由于probe的优先级比acquire的优先级高，那么A在L1 B中会无视acquire上的锁继续执行。

## 优先级与缓存组锁的具体实现

具体到优先级和锁的实现如下：

- 我们为每一个CacheArray都设立了一个vector数组 status，status的长度为set的大小，它(status)记录了每个缓存组的状态，目前我们只使用了3个bit（分别是第1、5、9个bit) ，第1个bit代表是否有acquire请求在工作，第5个bit请求代表是否有probe请求，第9个bit请求代表是否有release请求。如 status[idx] == 0x101，代表此时有acquire请求和release请求

  ```c++
  std::vector<uint32_t> status; 
  ```

- 线程的工作流程如下：当有一个请求到来时，它会首先获得要请求的缓存组的状态，如果当前的缓存组的优先级低于自己的优先级，那么线程会获得该缓存组的访问权，之后它需要重新设置当前的缓存组状态。

  - 考虑 **Case1**

    ​	当A线程在L1 B probe时，它发现status[idx] = 0x1（即只有一个acquire请求），而自己的优先级大于		acuqire（0x10>0x01)，因此它获得缓存组的访问权，并把status[idx]设为0x11

​        如果当前缓存组的优先级高于等于自己的优先级，那么线程会主动进行等待，直到线程的优先级小于自己请    	求的优先级。在请求结束后，每个线程需要再次修改当前缓存组的状态，将自己请求优先级对应的那1bit清0。

​    具体到代码实现上，我们为每一个CacheArray都设定了一个 mutex和一个信号量cv

```c++
std::mutex mtx;
std::condition_variable cv;
```

每个线程在使用（读/写）status之前都要先获得mtx的访问权（用于保障同时只有一个线程在使用（读/写）status），之后将请求的优先级和要请求使用的缓存组状态进行对比。

```c++
std::unique_lock lk(mtx, std::defer_lock);
lk.lock();
cv->wait(lk, [s, status, wait_value]{ return ((*status)[s] < wait_value)}); // 满足条件线程会继续运行，不满足条件则被挂起,之后线程后主动释放掉锁
(*status)[s] |= prority;  
lk.unlock();
```

线程在确定完成请求后需要再次访问status，重新修改缓存组的状态并唤起被挂起的线程

```c++
std::unique_lock lk(mtx, std::defer_lock);
lk.lock();
(*status)[s] &= ~prority;  
lk.unlock();
cv->notify_all(); // 唤醒挂起线程
```

## Acquire Ack

- 单线程的FlexiCAS，inner cache在向outer cache发出acquire请求后，outer cache只要完成acquire resp，那么它就可以释放掉缓存组锁了。但在多线程时并不是这样，在inner cache向outer cache发出acquire 请求时，outer meta的修改时间是早于inner meta的，考虑这种情况：

  **Case2**：

  - 一个二级的Cache系统，两个L1（分别为L1 A和 L1B，以下将它们对应的线程简称A和B)，以及一个L2
  - T0时刻，A和B访问地址X和地址Y，A先拿到L2的访问权，在修改完L2的meta后释放L2的缓存锁，B再拿到L2的缓存锁，并试图驱逐地址X，因此需要probe L1 A，由于probe的优先级高于acquire，因此B线程在L1 A probe时，有可能会出现 L1 A的meta还没有修改，B的probe就已经结束了(在 L1 A probe miss),最后就出现 L1 A中有地址X，L2没有地址X的错误情况(inclusive)

  为此，我们引入了Acquire ack机制：outer cache在acquire resp后不会立马释放掉，而是inner cache在修改完meta后，再向outer cache发出一个acquire_ack，outer cache在接收到ack后再释放掉缓存锁。在引入ack机制后重新考虑 **Case2**：B只会在L1 A的inner meta修改后才能获得缓存锁，再probe L1 A时一定会hit（如果probe 地址X的话）

## 缓存(cacheline)锁

- 在二级Cache只使用缓存组锁经过一部分测试并没有出现什么问题，但是在三级Cache只使用缓存组锁却不能让整个系统正常的运行，考虑以下这种情况：

  **Case3**:

  一个三级Cache，两个L1（A和B）连接一个L2，L2连接LLC

  - T0时刻，L2和LLC都有缓存块X
  - T1时刻，A想写地址X，B想flush地址X，那么A会在L2命中地址X，B会从LLC向L2发起probe请求（也会命中），因为probe的优先级比acquire高，那么B会再去probe L1 A和 L1 B，这时候就遇到了和我们在二级Cache遇到的类似问题：B如果在L1 A的meta还没有被修改前就probe了L1 A，那么在probe和acquire都结束后，就会出现L1 A有缓存块X而 L2 和 LLC都没有缓存块X的情况。

- 分析上述错误出现的原因是，由于A在L2会命中，所以A不会向LLC发出acquire 请求，但是B在L2 probe时由于优先级高于A，因此会直接进行probe，因此我们加入了缓存(cacheline)锁，它的工作机制如下：

  - 在acquire发生时，线程不仅对缓存组上锁，还要对具体的cacheline进行上锁，如果acquire具有足够的权限（即不需要向outer cache发出任何请求），那么它在 acquire请求结束后依次对cacheline和缓存组解锁
  - 如果acquire没有足够的权限，需要向outer cache发出请求，那么线程在向outer cache请求前会解锁缓存(cacheline)锁（**但不解锁缓存组锁**），允许probe操作，在从outer cache返回时需要再次锁上cacheline
  - 同样的，在probe发生时，线程不仅要判断缓存组是否上锁，还要判断 cacheline是否已经上锁。
  - 有争议的地方：release请求是否需要判断缓存锁？由于release是单向发起（即由inner cache发送给outer cache）,并且不会有任何额外的请求，因此不太会可能导致冲突的问题，因此release请求只需要判断缓存组上锁而不需要判断缓存锁的存在

- 当我们加上缓存锁机制后再次分析 **Case3**:

  - 由于A会在L2命中地址X并且不会向outer cache发出请求，那么B在L2进行probe时虽然拿到了缓存组的访问权限但没有拿到cacheline的访问权限，因此B只会在A释放掉缓存锁后再进行probe
  - 即使A在命中地址X后向outer cache发出请求（promote），那么B会在A向outer cache发出acquire请求得到cacheline的访问权，即使B驱逐了地址X，A在从outer cache返回后还是会重新初始化X，不会出现L1有X而L2没有X的错误情况

  
