// Bots for online matches (bots_web.cpp).
#pragma once
#include "vector.h"
class ConsoleFunctionClass;
void Rebel_Bots_Add_Commands(DynamicVectorClass<ConsoleFunctionClass *> &list);
void Rebel_Bots_Think(void);
int Rebel_Bots_Count(void);
