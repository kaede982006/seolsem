/*
 * conio.h - Console I/O header stub for vim on seolsem
 */

#ifndef _CONIO_H_SEOLSEM
#define _CONIO_H_SEOLSEM

/* Console functions - stubbed to use seolsem APIs */

/* These will be implemented in os_seolsem.c */
extern int kbhit(void);
extern int getch(void);
extern int getche(void);
extern int putch(int c);
extern char *cgets(char *str);
extern int cputs(const char *str);
extern int cprintf(const char *format, ...);

/* Screen functions */
extern void clrscr(void);
extern void clreol(void);
extern void gotoxy(int x, int y);
extern int wherex(void);
extern int wherey(void);

/* Text attribute functions */
extern void textcolor(int color);
extern void textbackground(int color);
extern void textattr(int attr);

/* Scroll/insert functions */
extern void insline(void);
extern void delline(void);

/* Window functions */
extern void window(int left, int top, int right, int bottom);

/* Screen info */
struct text_info {
    unsigned char winleft;
    unsigned char wintop;
    unsigned char winright;
    unsigned char winbottom;
    unsigned char attribute;
    unsigned char normattr;
    unsigned char currmode;
    unsigned char screenheight;
    unsigned char screenwidth;
    unsigned char curx;
    unsigned char cury;
};

extern void gettextinfo(struct text_info *ti);

/* Screen modes */
#define C80     3
#define BW80    2
#define MONO    7

/* Colors */
#define BLACK        0
#define BLUE         1
#define GREEN        2
#define CYAN         3
#define RED          4
#define MAGENTA      5
#define BROWN        6
#define LIGHTGRAY    7
#define DARKGRAY     8
#define LIGHTBLUE    9
#define LIGHTGREEN   10
#define LIGHTCYAN    11
#define LIGHTRED     12
#define LIGHTMAGENTA 13
#define YELLOW       14
#define WHITE        15
#define BLINK        128

#endif /* _CONIO_H_SEOLSEM */
