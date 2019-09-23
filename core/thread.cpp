#include "thread.h"
#include <unistd.h>
#include <time.h>
#include <list>
#include <unistd.h>
#include <errno.h>
using namespace std;


//调用栈关系
static co_env  env;

//用于保存所有协程
static std::deque<co_struct*> co_deques;

//准备就绪的协程跟正在运行的协程
static std::deque<co_struct*> work_deques;

//主进程上下文,主要用来保存切换的上下文
static co_struct co_main;

//协程休眠存放的链表
static std::list<time_co* > sleep_list;

//协程等待时需要用到,唤醒则在epoll_wait后.
std::list<co_struct *> wait_list;



void co_sleep(int sleep_time)
{
    time_co t_co;   
    t_co.co = get_current();
    gettimeofday(&t_co.tv,NULL);
    t_co.tv.tv_sec += sleep_time;
    t_co.co->status = Status::SLEEPING;
    sleep_list.push_back(&t_co);
    co_yield();

    LOG_DEBUG("co_sleep end");
}


void co_wake()
{
    struct timeval  tv;
    gettimeofday(&tv,NULL);   

    for(auto list_iter = sleep_list.begin(); list_iter != sleep_list.end();)
    {
        auto iter = list_iter++;
        if((*iter)->tv.tv_sec < tv.tv_sec)
        {
            LOG_DEBUG("prev.sec=%d, now.sec=%d",(*iter)->tv.tv_sec, tv.tv_sec);
            (*iter)->co->status = Status::READY;
            work_deques.push_back((*iter)->co);
            sleep_list.erase(iter);
            LOG_DEBUG("sleep_list.size=%d",sleep_list.size());
        }
        
    }
}



co_struct* get_current()
{
    return env.call_stack[env.index-1];
}


void co_func(co_struct* co)
{
    if(co->fun)
    {
        Fun fn = co->fun;
        fn(co->arg);
    }

    co_struct * now  = env.call_stack[env.index-1];
    now->is_end = true;
    LOG_DEBUG("over");
    co_yield();

}

void co_init()
{
    if(env.call_stack[0] == NULL)
    {
        getcontext(&co_main.context);
        co_main.co_id  = (size_t)&co_main;
        co_main.status = Status::RUNNING;
        env.call_stack[env.index++] = &co_main;
    }
}



int co_create(co_struct* &co, Fun func, void *arg)
{
  
    
    co = new co_struct;
    getcontext(&co->context);
    co->arg    = arg;
    co->fun    = func;
    co->co_id  = (size_t )co;
    co->status = Status::READY;
    co->context.uc_stack.ss_sp    = co->stack;
    co->context.uc_stack.ss_size  = Default_size;
    co->context.uc_stack.ss_flags = 0;
    co->context.uc_link = NULL;

    makecontext(&co->context, (void (*)())co_func, 1, co);  
    co_deques.push_back(co);

    return 0;
}

void ready_co_to_queue()
{
    if(!work_deques.empty())
        return;
    
    for(auto co: co_deques)
    {
        if(co->status == Status::READY)
        {
            LOG_DEBUG("co->id=%d",co->co_id);
            work_deques.push_back(co);
            LOG_DEBUG("work.size()=%d", work_deques.size());
        }
    }
}

void schedule()
{
    
    while(1)
    {
        
        co_wake();
        ready_co_to_queue();
    
        if(work_deques.empty() && sleep_list.empty() && wait_list.empty())
        {
            LOG_DEBUG("work_deues.empty, schedule finish");
            return;
        }
/*
       
*/     

        env.ev_manger.wait_event();
        for(int i = 0; i< env.ev_manger.active_num; ++i)
        {
            int fd = ev->active_ev[i].data.fd;
            for(auto list_item = wait_list.begin(); list_item != wait_list.end();)
            {
                if(list_item->ev.fd == fd)
                {
                    list_item->status = Status::READY;
                    list_item = wait_list.erase(list_item);
                    break;
                }
                else
                    ++ list_item;
            }

        }
        
        ready_co_to_queue();
        if(work_deques.empty() && (!sleep_list.empty() || !wait_list.empty()) )
        {
            continue;
        }
        else if(work_deques.empty() && sleep_list.empty() && wait_list.empty())
        {
            return;
        }
        


  
        co_struct * next_co = work_deques.front();
        work_deques.pop_front(); 
    
        LOG_DEBUG("next_co= %d", next_co->co_id);
        co_resume(next_co);

        if(next_co->status == Status::EXIT)
            co_releae(next_co);

        LOG_DEBUG("schedule end ");

    }

}



void  co_yield()
{
    co_struct * now  = env.call_stack[env.index -1];
    co_struct * prev = env.call_stack[env.index -2];
    LOG_DEBUG("will runing=%d",prev->co_id);
    env.index--;

    if(now->is_end == true)
        now->status = Status::EXIT;
    else if(now->status == Status::RUNNING) 
        now->status = Status::READY;

    prev->status = Status::RUNNING;
    swapcontext(&now->context, &prev->context);
    now->status = Status::RUNNING;

}



void co_resume(co_struct* co)
{
    if(co->status != Status::READY)
    {
        LOG_DEBUG("co->status != READY,=%d",co->status);
        return;
    }

    env.call_stack[env.index++] = co;

    auto * prev  = env.call_stack[env.index -2];
    co->status   = Status::RUNNING;    
    prev->status = Status::READY;
    swapcontext(&prev->context, &co->context);
    prev->status = Status::RUNNING;
   
}



void co_releae(co_struct* co)
{
    if(co->status != Status::EXIT)
        return;

    for(auto iter = co_deques.begin(); iter != co_deques.end();)
    {
        if(*iter == co)
        {
            iter = co_deques.erase(iter);
            delete co;
        }
        else
            iter++;
    }

    LOG_DEBUG("co_deques.size=%d", co_deques.size());
}



void ev_register_to_manager(int fd, int event,int ops)
{
    co_struct* now = get_current();
    now.ev.init_event(fd, event, ops);
    env.ev_manger.updateEvent(now->ev);
    now->status = Status::WAITING;
   co_yield();
}

int co_accept(int fd ,struct sockaddr* addr, socklen_t *len)
{
   
    int sockfd =-1;
    ev_register_to_manager(fd,EPOLLIN, EPOLL_CTL_ADD | EPOLLONESHOT);
    sockfd = accept(fd, addr, len);
	exit_if(sockfd < 0, "accept failed");
}



ssize_t co_recv(int fd, void *buf, size_t len, int flags) {
	

	ev_register_to_manager(fd, POLLIN | POLLERR | POLLHUP,EPOLL_CTL_ADD);

	int ret = recv(fd, buf, len, flags);
	if (ret < 0) {
		//if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET) return -1;
		//printf("recv error : %d, ret : %d\n", errno, ret);
		
	}
	return ret;
}