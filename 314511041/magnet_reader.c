#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>   // 包含 open 等巨集
#include <unistd.h>  // 包含 write, close, usleep 等函數
#include <stdint.h>

int main(){

    int fd = open("/dev/mag_sensor", O_RDONLY);
    if(fd < 0){
        printf("Cannot open /dev/mag_sensor\n");
        return -1;
    }
    uint8_t gpio_value;

    while (1)
    {
        read(fd, &gpio_value, 1);
        printf("Digital: %d\n", gpio_value);
        usleep(2e5);
    }
    
    close(fd);
    return 0;
}