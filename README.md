# state-threads-note

学习、阅读、抄写 state-threads 源码，但是有几个主要区别:
1. 删除一些平台相关的移植性和 debug 相关代码
2. 改使用 ucontext 而不是 setjmp 函数，因为后者在 linux 下要想做到栈隔离，需要使用汇编
3. 事件模块只实现 epoll
4. 记录了某些可能的改进项，不过这不代表是必要的改进，因为 state-thread 的使用场景是确定的，
   也并不是要实现一个通用化的线程库