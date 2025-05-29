#include "ksignal.h"

#include <defs.h>
#include <proc.h>
#include <trap.h>

// 外部声明
extern struct proc *pool[NPROC];

// 初始化信号
int siginit(struct proc *p) {
    for (int i = SIGMIN; i <= SIGMAX; i++) {
        p->signal.sa[i].sa_sigaction = SIG_DFL;
        sigemptyset(&p->signal.sa[i].sa_mask);
        p->signal.sa[i].sa_restorer = NULL;
        // 初始化 siginfo 结构体
        p->signal.siginfos[i].si_signo = 0;
        p->signal.siginfos[i].si_code = 0;
        p->signal.siginfos[i].si_pid = 0;
        p->signal.siginfos[i].si_status = 0;
        p->signal.siginfos[i].addr = NULL;
    }
    sigemptyset(&p->signal.sigmask);
    sigemptyset(&p->signal.sigpending);
    return 0;
}
// 复制父进程的信号处理配置
int siginit_fork(struct proc *parent, struct proc *child) {
    memmove(&child->signal.sa[1], &parent->signal.sa[1], sizeof(struct sigaction) * SIGMAX);
    child->signal.sigmask = parent->signal.sigmask;
    sigemptyset(&child->signal.sigpending); // 清空挂起信号
    return 0;
}
// 重置信号处理方式（保留被忽略的）
int siginit_exec(struct proc *p) {
    for (int i = SIGMIN; i <= SIGMAX; i++) {
        if (p->signal.sa[i].sa_sigaction != SIG_IGN) {
            p->signal.sa[i].sa_sigaction = SIG_DFL;
        }
    }
    return 0;
}
// 根据设计对信号进行相应处理
int do_signal(void) {
    struct proc *p = curr_proc();
    
    // 首先检查是否有子进程变成了ZOMBIE状态，如果有则发送SIGCHLD信号
    // 这个检查需要在处理其他信号之前进行，确保SIGCHLD能够及时发送
    for (int i = 0; i < NPROC; i++) {
        struct proc *child = pool[i];
        if (child && child->parent == p && child->state == ZOMBIE) {
            // 只有当前没有pending的SIGCHLD信号时才发送新的
            if (!(p->signal.sigpending & sigmask(SIGCHLD))) {
                // 确定退出代码：如果child->killed < 0说明是被信号终止的，使用killed值
                // 否则使用exit_code
                int exit_code = (child->killed < 0) ? child->killed : child->exit_code;
                // 设置 SIGCHLD 信号为挂起状态
                sigaddset(&p->signal.sigpending, SIGCHLD);
                // 填充 siginfo_t 结构体
                p->signal.siginfos[SIGCHLD].si_signo = SIGCHLD;
                p->signal.siginfos[SIGCHLD].si_code = exit_code;
                p->signal.siginfos[SIGCHLD].si_pid = child->pid;
                p->signal.siginfos[SIGCHLD].si_status = 0;
                p->signal.siginfos[SIGCHLD].addr = NULL;
                break; // 一次只处理一个子进程
            }
        }
    }
    
    // 现在处理所有pending的信号（包括刚刚可能添加的SIGCHLD）
    for (int signo = SIGMIN; signo <= SIGMAX; signo++) {
        if ((p->signal.sigpending & sigmask(signo)) && !(p->signal.sigmask & sigmask(signo))) {
            sigaction_t *sa = &p->signal.sa[signo];
            // SIGKILL 直接终止
            if (signo == SIGKILL) {
                setkilled(p, -10 - signo);
                sigdelset(&p->signal.sigpending, signo);
                return 0;
            }
            // SIGSTOP 暂停进程 - 不能被捕获或忽略
            if (signo == SIGSTOP) {
                acquire(&p->lock);
                p->state = SLEEPING; // 将进程状态设为SLEEPING以暂停执行
                sigdelset(&p->signal.sigpending, signo);
                sched(); // 调用调度器，让出CPU
                release(&p->lock);
                return 0;
            }
            // SIGCONT 恢复进程 - 不能被捕获或忽略  
            if (signo == SIGCONT) {
                // SIGCONT在sys_sigkill中立即处理，这里只是清除pending信号
                sigdelset(&p->signal.sigpending, signo);
                // SIGCONT处理完后继续处理其他信号，不return
            }
            // 对于忽略信号，不进行处理
            if (sa->sa_sigaction == SIG_IGN) {
                sigdelset(&p->signal.sigpending, signo);
            } else if (sa->sa_sigaction == SIG_DFL) {
                setkilled(p, -10 - signo);
                sigdelset(&p->signal.sigpending, signo);
            } else {
                struct trapframe *tf = p->trapframe;
                // 计算新栈指针，需要额外空间存储 siginfo_t
                uint64 new_sp = tf->sp - sizeof(struct trapframe) - sizeof(sigset_t) - sizeof(siginfo_t); 
                // 将 trapframe、oldmask 和 siginfo 拷贝到用户栈
                sigset_t oldmask = p->signal.sigmask;
                
                // 构造 siginfo_t 结构体
                siginfo_t siginfo;
                siginfo.si_signo = signo;
                siginfo.si_code = p->signal.siginfos[signo].si_code; // 使用存储的code而不是硬编码0
                siginfo.si_pid = p->signal.siginfos[signo].si_pid; // 使用存储的发送者 pid
                siginfo.si_status = 0;
                siginfo.addr = NULL;
                
                acquire(&p->mm->lock);
                // 复制trapframe到用户栈
                copy_to_user(p->mm, new_sp, (char*)tf, sizeof(struct trapframe));
                // 复制oldmask到用户栈
                copy_to_user(p->mm, new_sp + sizeof(struct trapframe), (char*)&oldmask, sizeof(sigset_t));
                // 复制siginfo到用户栈
                copy_to_user(p->mm, new_sp + sizeof(struct trapframe) + sizeof(sigset_t), (char*)&siginfo, sizeof(siginfo_t));
                release(&p->mm->lock);
                // 处理信号
                p->signal.sigmask |= sa->sa_mask;
                sigaddset(&p->signal.sigmask, signo);
                sigdelset(&p->signal.sigpending, signo);
                // 设置trapframe跳转到handler
                tf->sp = new_sp; // 更新栈指针到备份数据顶部
                tf->epc = (uint64)sa->sa_sigaction;
                tf->a0 = signo; // 第一个参数：信号号
                tf->a1 = new_sp + sizeof(struct trapframe) + sizeof(sigset_t); // 第二个参数：siginfo_t 指针
                tf->ra = (uint64)sa->sa_restorer; // 返回地址
                break;
            }
        }
    }
    return 0;
}

// 内核发送信号的辅助函数
void send_kernel_signal(struct proc *p, int signo, int code) {
    sigaddset(&p->signal.sigpending, signo);
    // 设置内核发送信号的 siginfo
    p->signal.siginfos[signo].si_signo = signo;
    p->signal.siginfos[signo].si_code = code;
    p->signal.siginfos[signo].si_pid = -1; // 内核发送的信号 pid 为 -1
    p->signal.siginfos[signo].si_status = 0;
    p->signal.siginfos[signo].addr = NULL;
}

// syscall handlers:
//  sys_* functions are called by syscall.c

int sys_sigaction(int signo, const sigaction_t __user *act, sigaction_t __user *oldact) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    if (oldact) {
        copy_to_user(p->mm, (uint64)oldact, (char*)&p->signal.sa[signo], sizeof(sigaction_t));
    }
    if (act) {
        sigaction_t sa;
        copy_from_user(p->mm, (char*)&sa, (uint64)act, sizeof(sigaction_t));
        p->signal.sa[signo] = sa;
        
        // 如果设置的是SIGCHLD处理函数，主动检查一次是否有ZOMBIE子进程
        if (signo == SIGCHLD && sa.sa_sigaction != SIG_DFL && sa.sa_sigaction != SIG_IGN) {
            release(&p->mm->lock);
            check_sigchld();
            acquire(&p->mm->lock);
        }
    }
    release(&p->mm->lock);
    return 0;
}

int sys_sigreturn() {
    struct proc *p = curr_proc();
    struct trapframe *tf = p->trapframe;
    sigset_t oldmask;
    acquire(&p->mm->lock);
    // 从用户栈复制trapframe
    copy_from_user(p->mm, (char*)tf, tf->sp, sizeof(struct trapframe));
    // 从用户栈复制oldmask
    copy_from_user(p->mm, (char*)&oldmask, tf->sp + sizeof(struct trapframe), sizeof(sigset_t));
    // 注意：我们不需要恢复 siginfo_t，它只是作为参数传递给处理函数
    release(&p->mm->lock);
    p->signal.sigmask = oldmask;
    return 0;
}

int sys_sigprocmask(int how, const sigset_t __user *set, sigset_t __user *oldset) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    if (oldset) {
        copy_to_user(p->mm, (uint64)oldset, (char*)&p->signal.sigmask, sizeof(sigset_t));
    }
    if (set) {
        sigset_t newmask;
        copy_from_user(p->mm, (char*)&newmask, (uint64)set, sizeof(sigset_t));
        // SIGKILL不能被mask
        sigdelset(&newmask, SIGKILL);
        switch (how) {
            case SIG_BLOCK:
                p->signal.sigmask |= newmask;
                break;
            case SIG_UNBLOCK:
                p->signal.sigmask &= ~newmask;
                break;
            case SIG_SETMASK:
                p->signal.sigmask = newmask;
                break;
            default:
        }
    }
    release(&p->mm->lock);
    return 0;
}

int sys_sigpending(sigset_t __user *set) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    copy_to_user(p->mm, (uint64)set, (char*)&p->signal.sigpending, sizeof(sigset_t));
    release(&p->mm->lock);
    return 0;
}

int sys_sigkill(int pid, int signo, int code) {
    struct proc *p;
    struct proc *sender = curr_proc(); // 获取发送者进程
    for (int i = 0; i < NPROC; i++) {
        p = pool[i];
        if (p->pid == pid && p->state != UNUSED) {
            sigaddset(&p->signal.sigpending, signo);
            // 记录发送者信息到 siginfo
            p->signal.siginfos[signo].si_signo = signo;
            p->signal.siginfos[signo].si_code = code;
            p->signal.siginfos[signo].si_pid = sender ? sender->pid : -1; // 如果有发送者则记录pid，否则为-1（内核发送）
            p->signal.siginfos[signo].si_status = 0;
            p->signal.siginfos[signo].addr = NULL;
            
            // 特殊处理：如果发送的是SIGCONT且目标进程处于SLEEPING状态，立即唤醒它
            if (signo == SIGCONT && p->state == SLEEPING) {
                acquire(&p->lock);
                p->state = RUNNABLE;
                add_task(p); // 将进程重新添加到任务队列
                release(&p->lock);
                // 直接清除SIGCONT信号，无需pending
                sigdelset(&p->signal.sigpending, signo);
                return 0;
            }
            
            return 0;
        }
    }
    return -1;
}

// 主动检查并发送SIGCHLD信号的函数
void check_sigchld(void) {
    struct proc *p = curr_proc();
    
    // 检查是否有子进程变成了ZOMBIE状态
    for (int i = 0; i < NPROC; i++) {
        struct proc *child = pool[i];
        if (child && child->parent == p && child->state == ZOMBIE) {
            // 只有当前没有pending的SIGCHLD信号时才发送新的
            if (!(p->signal.sigpending & sigmask(SIGCHLD))) {
                // 确定退出代码：如果child->killed < 0说明是被信号终止的，使用killed值
                // 否则使用exit_code
                int exit_code = (child->killed < 0) ? child->killed : child->exit_code;
                // 设置 SIGCHLD 信号为挂起状态
                sigaddset(&p->signal.sigpending, SIGCHLD);
                // 填充 siginfo_t 结构体
                p->signal.siginfos[SIGCHLD].si_signo = SIGCHLD;
                p->signal.siginfos[SIGCHLD].si_code = exit_code;
                p->signal.siginfos[SIGCHLD].si_pid = child->pid;
                p->signal.siginfos[SIGCHLD].si_status = 0;
                p->signal.siginfos[SIGCHLD].addr = NULL;
                break; // 一次只处理一个子进程
            }
        }
    }
}