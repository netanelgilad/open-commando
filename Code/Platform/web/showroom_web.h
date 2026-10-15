// The online front screen (showroom_web.cpp).
#pragma once
bool Rebel_Showroom_Active(void);
void Rebel_Showroom_Render(void);
void Rebel_Showroom_Run_Pending(void);
// --showroom: skip the intro movies and open the showroom with the main menu.
extern bool Rebel_Showroom_Startup;
void Rebel_Showroom_Enter_Now(void);
