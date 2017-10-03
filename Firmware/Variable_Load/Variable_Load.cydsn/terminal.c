#include <stdio.h>
#include "Project.h"

void goToPos(int x, int y)
{
  char buffer[16];
  sprintf(buffer, "\x1b[%d;%dH", y, x);
  USBUART_PutString(buffer);
}

void cls(void)
{
  USBUART_PutString("\x1b[2J");
}

void init(void)
{
  USBUART_PutString("\x1b\x63");
}