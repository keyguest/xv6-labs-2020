#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void sieve(int pleft[2]) {
    // read data
    int buf_first;
    read(pleft[0], &buf_first, sizeof(buf_first));
    // the data is empty
    if(buf_first == -1) {
        exit(0);
    }
    printf("prime %d\n",buf_first);
    int pright[2];
    pipe(pright);
    if(fork() == 0){
        // son process
        close(pright[1]);
        close(pleft[0]);
        sieve(pright);
    }else {
        int buf;
        // parent process
        close(pright[0]);
        while(read(pleft[0], &buf, sizeof(buf)) && buf != -1) {
            // judge and wirte data
            if(buf % buf_first == 0) continue;
            write(pright[1],  &buf, sizeof(buf));
        }
        buf = -1;
        write(pright[1], &buf, sizeof(buf));
        close(pright[1]);
        wait(0);
        exit(0);
    }
}



int main() {

    int p_first[2];
    pipe(p_first);
    // first write data to pipe
    if(fork() == 0) {
        close(p_first[1]);
        sieve(p_first);
    }else {
        int i;
        // write first data
        close(p_first[0]);
        for(i=2;i<=35;i++) {
            write(p_first[1], &i, sizeof(i));
        }
        i = -1;
        write(p_first[1], &i, sizeof(i));
        
        close(p_first[1]);

        wait(0);// wait son process exit
    }



    exit(0);
}