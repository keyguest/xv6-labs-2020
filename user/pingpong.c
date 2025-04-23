#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(){
    int p_fa[2], p_son[2];
    pipe(p_fa); // let father to write
    pipe(p_son);// let son to write

    int pid = fork();
    if(pid == 0) {
        char buf;
        // son process
        // son read pipe
        close(0);
        dup(p_fa[0]);// write read_fd to 0 pos
        close(p_fa[0]); // reduce expenses
        close(p_fa[1]);
        read(0, &buf, 1); // wait to read relative msg
        printf("%d: received ping\n", getpid());
        // son write pipe
        close(1);
        dup(p_son[1]);
        close(p_son[1]);
        close(p_son[0]);
        write(1,"0",2);
    }else {
        char buf;
        // father write pipe
        close(p_fa[0]);
        write(p_fa[1],"0",1);
        close(p_fa[1]);
        // father read pipe
        close(p_son[1]);
        read(p_son[0], &buf, 1); // wait to read relative msg
        printf("%d: received pong\n", getpid());
        close(p_son[0]);
        wait(0);
    }

    
    exit(0);
}