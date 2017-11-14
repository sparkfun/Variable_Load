#include <stdio.h>
#include "project.h"
#include "terminal.h"

void goToPos(int x, int y)
{
  char buffer[16];
  sprintf(buffer, "\x1b[%d;%df", y, x);
  putString(buffer);
}

void cls(void)
{
  putString("\x1b[2J");
}

void init(void)
{
  putString("\x1b\x63");
}

void putString(const char *buffer)
{
  if (0 != (USBUART_LINE_CONTROL_DTR & USBUART_GetLineControl()) )
  {
    while (0u == USBUART_CDCIsReady());
    USBUART_PutString(buffer);
  }
}