#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>
#include <bcm2835.h>
#include "camera.h"
#include "graphics.h"
#include <ncurses.h>
#include <sys/time.h>
#include <pthread.h>
#include <signal.h>
#include "config.h"

//should the camera convert frame data from yuv to argb automatically?
#define DO_ARGB_CONVERSION true
//how many detail levels (1 = just the capture res, > 1 goes down by half each level, 4 max)
#define NUM_LEVELS 1

//raw FPS of the camera
#define FPS 30
#define laser_freq FPS / 3
#define laser_duty_cycle .4

//laser control pin
#define PIN RPI_GPIO_P1_12

//distance to virtual camera plane (px)
#define VD 633

//physical height from camera to laser (mm)
#define H 100

//output serial port
#define PORT "/dev/ttyAMA0" 

//edge protection margin
#define DM 3

//buffer for serial output
unsigned char buf[3];

//buffer for subpixel peak detection
short spbuf[7];

void revokeRoot();
void cleanup();
void catch_SIGINT(int sig);
void* laserloop(void *arg);

bool need_cleanup = false;
bool sensor_active = true;

GLubyte data_buf[CAMERA_WIDTH * CAMERA_HEIGHT * 4];
short max_value[CAMERA_WIDTH];
short max_index[CAMERA_WIDTH];
int totals[CAMERA_WIDTH];
int total_diffs[CAMERA_WIDTH];

short xpos[CAMERA_WIDTH];
short dpos[CAMERA_WIDTH];
int pcount;

GLubyte out_tex_buf[CAMERA_WIDTH * CAMERA_HEIGHT * 4];

int main(int argc, const char **argv)
{
  //Do we want terminal output
  unsigned char display = argc > 1 && !strcmp(argv[1],"display");
  printf("DISPLAY: %s\n",display ? "TRUE" : "FALSE");

  //Init the laser output
  if(!bcm2835_init())
    return 1;

  signal(SIGINT, catch_SIGINT);

  //Setting up threads
  pthread_t laserthread;
  pthread_create(&laserthread, NULL, &laserloop, NULL);

  //Setting up serial output
  struct termios tio;
  int tty_fd;

  memset(&tio,0,sizeof(tio));
  tio.c_iflag=0;
  tio.c_oflag=0;
  tio.c_cflag=CS8|CREAD|CLOCAL;           // 8n1, see termios.h for more information
  tio.c_lflag=0;
  tio.c_cc[VMIN]=1;
  tio.c_cc[VTIME]=5;
  
  tty_fd=open(PORT, O_RDWR | O_NONBLOCK);      
  cfsetospeed(&tio,B115200);            // 115200 baud
  cfsetispeed(&tio,B115200);            // 115200 baud
  
  tcsetattr(tty_fd,TCSANOW,&tio);

  revokeRoot();

  //init graphics and the camera
  InitGraphics();
  CCamera* cam = StartCamera(CAMERA_WIDTH, CAMERA_HEIGHT,FPS,NUM_LEVELS,DO_ARGB_CONVERSION);

  GfxTexture textures[5];
  for(int i = 0;i < 5;i++) {
    textures[i].Create(CAMERA_WIDTH,CAMERA_HEIGHT);
  }

  for(int i = 0;i < CAMERA_HEIGHT;i++) {
    for(int j = 0;j < CAMERA_WIDTH;j++) {
      out_tex_buf[((i * CAMERA_WIDTH) + j) * 4 + 0] = 0;
      out_tex_buf[((i * CAMERA_WIDTH) + j) * 4 + 1] = 0;
      out_tex_buf[((i * CAMERA_WIDTH) + j) * 4 + 2] = 0;
      out_tex_buf[((i * CAMERA_WIDTH) + j) * 4 + 3] = 0;
    }
  }

  struct timeval time;
  gettimeofday(&time, NULL);
  unsigned long old_time = time.tv_sec * 1000000 + time.tv_usec;
  unsigned long new_time;
  int pos = 0;
  float uspf = 33;

  if(display) {
    //Init ncurses
    initscr();      /* initialize the curses library */
    keypad(stdscr, TRUE);  /* enable keyboard mapping */
    nonl();         /* tell curses not to do NL->CR/NL on output */
    cbreak();       /* take input chars one at a time, no wait for \n */
    clear();
    nodelay(stdscr, TRUE);
  }

  unsigned char cur_frame = 0;

  printf("Running frame loop\n");
  for(;!need_cleanup;) {
    if(read(tty_fd,buf,1) > 0) {
      if(buf[0] == 'r') {
        sensor_active = true;
      }
      if(buf[0] == 's') {
        sensor_active = false;
      }
    }

    if(!sensor_active) {
      usleep(1000);
      continue;
    }

    int down_sample = 0;

    //spin until we have a camera frame
    const void* frame_data; int frame_sz;
    while(!cam->BeginReadFrame(0,frame_data,frame_sz)) {};

    textures[cur_frame].SetPixels(frame_data);

    cam->EndReadFrame(down_sample);

    if(cur_frame == 2) {
      //begin frame, draw the texture then end frame
      //DrawTextureRect(textures,(GLvoid*)data_buf);
      BeginFrame();
      InitDrawRect();
      DrawTextureRect(textures);
      FinishDrawRect((GLvoid*)data_buf);
      EndFrame();

      //Finding the max position and value
      for(int j = 0;j < CAMERA_WIDTH;j++) {
        max_value[j] = 0;
        max_index[j] = -1;
        totals[j] = 0;
        total_diffs[j] = 0;
      }
      for(int j = 0;j < CAMERA_WIDTH;j++) {
        short prev_value;
        for(int i = DM;i < CAMERA_HEIGHT / 2 - DM;i++) {
          short value = data_buf[((i * CAMERA_WIDTH) + j) * 4 + 2];
          totals[j] += value;
          if(i > DM) {
            total_diffs[j] += abs(value - prev_value);
          }
          prev_value = value;
          if(value > max_value[j]) {
            max_value[j] = value;
            max_index[j] = CAMERA_HEIGHT - i - 1;
          }
        }
      }

      //Displaying the found value if it is above a threshold
      //Also output to serial port
      pcount = 0;
      for(int j = 0;j < CAMERA_WIDTH;j++) {
        //Zero out lower part byte
        //if(max_value[j] > (25.0 / 640) * totals[j]) {
        //if(max_value[j] > 6) {
        if(max_value[j] * 40 > totals[j] + total_diffs[j]) {
          // printf("%i %i\n",totals[j],total_diffs[j]);
          //Diagnostics display
          out_tex_buf[((max_index[j] * CAMERA_WIDTH) + j) * 4 + 1] = 255;

          int output = max_index[j] - CAMERA_HEIGHT / 2;
          if(output > 254) output = 254;
          output *= 16;

          //Running subpixel peak detection
          //COM7
          int total = 0;
          for(int i = 0;i < 7;i++) {
            spbuf[i] = data_buf[(((CAMERA_HEIGHT - 1 - (max_index[j] + i - 3)) * CAMERA_WIDTH) + j) * 4 + 2];
            total += spbuf[i];
          }
          //printf("%i: %i %i %i %i %i %i %i\n",max_value[j],spbuf[0],spbuf[1],spbuf[2],spbuf[3],spbuf[4],spbuf[5],spbuf[6]);
          //Scaled from 16->a larger number to counteract the sharper peak from shaders
          output += (3 * spbuf[6] + 2 * spbuf[5] + spbuf[4] - spbuf[2] - 2 * spbuf[1] - 3 * spbuf[0]) * 32 / total;

          if(j % 2) {
            buf[2] = (int)(output/16);
            buf[0] |= (output << 4) & 0xF0;
          }
          else {
            buf[1] = (int)(output/16);
             buf[0] = output & 0x0F;
          }
        }
        else {
          if(j % 2) {
            buf[2] = 254;
          }
          else {
            buf[1] = 254;
          }
        }
        //Output last 2 bytes
        if(j % 2) {
          write(tty_fd,buf,3);
        }
      }
      textures[3].SetPixels(out_tex_buf);
      for(int j = 0;j < CAMERA_WIDTH;j++) {
          out_tex_buf[((max_index[j] * CAMERA_WIDTH) + j) * 4 + 1] = 0;
      }
      textures[4].SetPixels(data_buf);
      buf[2] = 255;
      buf[1] = 255;
      buf[0] = 255;
      write(tty_fd,buf,3);
    }

    cur_frame++;
    cur_frame %= 3;
     
    gettimeofday(&time, NULL);
    new_time = time.tv_sec * 1000000 + time.tv_usec;
    int max = (new_time - old_time) / 1000;
    old_time = new_time;

    if(display) {
      mvprintw(0,0,"CURRENT fps: %.2f",1000.0 / (uspf = (uspf*.99 + max*.01)));
      //printf("CURRENT fps: %.2f",1000.0 / (uspf = (uspf*.99 + max*.01)));
      refresh();
    }
  }

  if(display) {
    endwin();
  }
  StopCamera();
  close(tty_fd);
  return 0;
}

void* laserloop(void *arg) {
  //Setting up timer stuff
  struct timeval tv;
  gettimeofday(&tv,NULL);
  unsigned long old_time = 1000000 * tv.tv_sec + tv.tv_usec;
  unsigned long init_time = old_time;

  //Setting laser pin as output
  bcm2835_gpio_fsel(PIN, BCM2835_GPIO_FSEL_OUTP);

  bool laser_state = false;

  for(;!need_cleanup;) {
    if(!sensor_active) {
      bcm2835_gpio_write(PIN, LOW);
      usleep(1000);
      continue;
    }

    laser_state = !laser_state;
    bcm2835_gpio_write(PIN, laser_state ? HIGH : LOW);

    //Timer loop code
    gettimeofday(&tv,NULL);
    unsigned long cur_time = 1000000 * tv.tv_sec + tv.tv_usec;
    unsigned long time_diff = 1000000L/(laser_freq) * (laser_state ? laser_duty_cycle : (1 - laser_duty_cycle));
    if(old_time + time_diff > cur_time) {
      usleep(old_time + time_diff - cur_time);
    }
    old_time += time_diff;
  }
  bcm2835_gpio_write(PIN, LOW);
  return 0;
}

void cleanup() {
}

void catch_SIGINT(int sig) {
  need_cleanup = true;
}

void revokeRoot()
{
  if (getuid () + geteuid () == 0)      // Really running as root
    return ;

  if (geteuid () == 0)                  // Running setuid root
    seteuid (getuid ()) ;               // Change effective uid to the uid of the caller
}
