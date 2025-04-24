#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

// 执行程序
void run(char* program, char* args[]) {
    if(fork() == 0) {
        exec(program, args);
    }
    return;
}

int main(int argc, char* argv[]) {
    char buf[2048];
    char* p = buf, *st_pos = buf;
    char* base_arg[128];
    char** arg_pos = base_arg;

    // 1.解析xargs命令参数，第一个参数即要执行的命令
    for (int i = 1; i < argc; i++){
        *arg_pos = argv[i];
        arg_pos++;
    }
    
    // 2.读取前置命令执行结果（默认放在文件描述符0【终端写入位置】中）
    char** extra_p = arg_pos; // 用于记录额外的位置指针
    while(read(0, p, 1) != 0) {
        if(*p == '\n' || *p == ' ') {
            char* temp = p;
            *p = '\0';// 一个参数结束
            *extra_p = st_pos;
            extra_p++;
            st_pos = p + 1;

            if(*temp == '\n') {
                *extra_p = 0;// 终止
                run(argv[1], base_arg);
                extra_p = arg_pos;
            }
        }
        p++;
    }
    if(extra_p != arg_pos) {
        *p = '\0';
        *extra_p = st_pos;
        extra_p++;
        *extra_p = 0;// 终止
        run(argv[1], base_arg);
    } 
    
    while(wait(0) != -1);

    exit(0);
}