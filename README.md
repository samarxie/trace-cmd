# trace-cmd
1. 如何编译？
   在trace-cmd跟目录下执行：make LDFLAGS=-static CC=arm-none-linux-gnueabi-gcc trace-cmd
2. 如何使用？
   将编译好的trace-cmd push到手机里面，查看trace-cmd使用方式，网上很多介绍，不多说.
3. 如何解析生成的trace.dat文件？
   在PC机器上面安装kenrelshark来解析，至于使用何种cmd来解析网络很多.
4. trace-cmd-old
   是之前网络上
5. trace-cmd-new
   是官方的，修改很多，而且与社区可以保持同步update.
   

new trace-cmd download and update 方法如下：
1. git://git.kernel.org/pub/scm/linux/kernel/git/rostedt/trace-cmd.git
2. https://git.kernel.org/pub/scm/linux/kernel/git/rostedt/trace-cmd.git
3. https://kernel.googlesource.com/pub/scm/linux/kernel/git/rostedt/trace-cmd.git
