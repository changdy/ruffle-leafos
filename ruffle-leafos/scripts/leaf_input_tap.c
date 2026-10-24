/* Development test only: send one bounded tap through the built-in evdev path.
 * Never installed in the pak. Identity and neutral keys are checked first.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void stop(int sig) { (void)sig; interrupted = 1; }
static int event(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event packet[2] = {{.type=type, .code=code, .value=value}, {.type=EV_SYN, .code=SYN_REPORT}};
    return write(fd, packet, sizeof(packet)) == sizeof(packet) ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: leaf-input-tap /dev/input/eventN guide|a|b|x|y|l|r|start|select|up|down|left|right\n"); return 2; }
    unsigned short type=EV_KEY, code=0;
    int value=1;
    if (!strcmp(argv[2], "guide")) code=BTN_MODE;
    else if (!strcmp(argv[2], "a")) code=BTN_EAST; /* target physical A, normalized once by frontend */
    else if (!strcmp(argv[2], "b")) code=BTN_SOUTH;
    else if (!strcmp(argv[2], "x")) code=BTN_NORTH;
    else if (!strcmp(argv[2], "y")) code=BTN_WEST;
    else if (!strcmp(argv[2], "l")) code=BTN_TL;
    else if (!strcmp(argv[2], "r")) code=BTN_TR;
    else if (!strcmp(argv[2], "start")) code=BTN_START;
    else if (!strcmp(argv[2], "select")) code=BTN_SELECT;
    else if (!strcmp(argv[2], "up")) {type=EV_ABS; code=ABS_HAT0Y; value=-1;}
    else if (!strcmp(argv[2], "down")) {type=EV_ABS; code=ABS_HAT0Y;}
    else if (!strcmp(argv[2], "right")) {type=EV_ABS; code=ABS_HAT0X;}
    else if (!strcmp(argv[2], "left")) {type=EV_ABS; code=ABS_HAT0X; value=-1;}
    else return 2;
    int fd=open(argv[1], O_RDWR|O_CLOEXEC);
    if (fd < 0) {perror("input open"); return 1;}
    char name[128]={0}, phys[128]={0};
    struct input_id id={0};
    unsigned long keys[(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))]={0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name)<0 || ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys)<0 ||
        ioctl(fd, EVIOCGID, &id)<0 || strcmp(name,"Loong Gamepad") ||
        strcmp(phys,"loong1_joypad/input0") || id.vendor!=0x9903 || id.product!=0x9913 ||
        ioctl(fd, EVIOCGKEY(sizeof(keys)), keys)<0) {
        fprintf(stderr,"unexpected input device identity\n"); close(fd); return 1;
    }
    for (int k=0;k<=KEY_MAX;k++) if (keys[k/(8*sizeof(unsigned long))] & (1UL<<(k%(8*sizeof(unsigned long))))) {
        fprintf(stderr,"physical button %d is held; refusing test tap\n",k); close(fd); return 1;
    }
    signal(SIGINT,stop); signal(SIGTERM,stop);
    if(event(fd,type,code,value)) {perror("input press"); close(fd); return 1;}
    usleep(120000);
    int result=event(fd,type,code,0);
    close(fd);
    if(result) {perror("input release"); return 1;}
    printf("physical_evdev_tap=%s released=true interrupted=%d\n",argv[2],(int)interrupted);
    return 0;
}
