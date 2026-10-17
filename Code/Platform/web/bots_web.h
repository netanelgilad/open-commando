// Bots for online matches (bots_web.cpp).
#pragma once
#include "vector.h"
class ConsoleFunctionClass;
void Rebel_Bots_Add_Commands(DynamicVectorClass<ConsoleFunctionClass *> &list);
void Rebel_Bots_Think(void);
int Rebel_Bots_Count(void);
class WideStringClass;
// Every chat line the server relays (sctextobj.cpp), for the bots' brain.
void Rebel_Bots_On_Chat(int sender, int type, const WideStringClass &text);
