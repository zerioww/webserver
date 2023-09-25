#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include"lock.h"
#include"threadpool.h"
#include<iostream>
#include<signal.h>
#include"http_conn.h"

#define MAX_FD 65535 //最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000

using namespace std;


//添加信号捕捉
void addsig(int sig,void(handler)(int)){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig,&sa,NULL);
}

//添加文件描述符添加到epoll对象中
extern void addfd(int epollfd,int fd,bool one_shot);
//从epoll中删除文件描述符
extern void removefd(int epollfd,int fd);
//修改文件描述符
extern void modfd(int epollfd,int fd,int ev);

int main(int argc,char *argv[]){
    
    if(argc<=1){
        cout<<"按照以下格式运行"<<basename(argv[0])<<" port_number"<<endl;
        exit(-1);
    }
    
    //获取端口号
    int port=atoi(argv[1]);

    //对SIGPIPE信号进行处理
    addsig(SIGPIPE,SIG_IGN);

    //创建线程池初始化信息
    threadpool<http_conn> * pool=NULL;
    try{
        pool=new threadpool<http_conn>;
    }catch(...){
        exit(-1);
    }

    //创建一个数组用于保护所有的客户端信息
    http_conn * users=new http_conn[MAX_FD];

    //创建监听的套接字
    int lfd=socket(AF_INET,SOCK_STREAM,0);
    if(lfd==-1){
        throw exception();
    }
    
    //设置端口复用
    int reuse=1;
    setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定
    struct sockaddr_in addr;
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(port);
    int ret=bind(lfd,(struct sockaddr *)&addr,sizeof(addr));
    if(ret==-1){
        throw exception();
    }

    //监听
    listen(lfd,5);

    //创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epfd=epoll_create(5);

    //将监听的文件描述符添加到epoll对象中
    addfd(epfd,lfd,false);
    http_conn::m_epollfd=epfd;

    while(true){
        int num=epoll_wait(epfd,events,MAX_EVENT_NUMBER,-1);
        if((num<0)&&(errno!=EINTR)){
            cout<<"epoll failure"<<endl;
            break;
        }

        //循环遍历事件数组
        for(int i=0;i<num;i++){
            int sockfd=events[i].data.fd;
            if(sockfd==lfd){
                //有客户端连接进来
                struct sockaddr_in cliaddr;
                socklen_t cliaddlen=sizeof(cliaddr);
                int connfd=accept(lfd,(struct sockaddr*)&cliaddr,&cliaddlen);

                if(http_conn::m_user_count>=MAX_FD){
                    //目前连接数满了
                    //给客户端写一个信息：服务器内部正忙
                    close(connfd);
                    continue;
                }
                //将新的客户数据初始化，放在数组中
                users[connfd].init(connfd,cliaddr);
            }
            else if(events[i].events &(EPOLLRDHUP|EPOLLHUP|EPOLLERR)){
                //对方异常断开或者错误事件
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN){
                if(users[sockfd].read()){//一次性把所有数据都读完
                    pool->append(users+sockfd);
                }
                else{
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT){
                if(!users[sockfd].write()){//一次性写完所有数据
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epfd);
    close(lfd);
    delete []users;
    delete pool;

    return 0;
}