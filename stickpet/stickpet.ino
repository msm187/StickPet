/*
 * stickpet.ino  —  a virtual pet + idle RPG for the M5StickS3
 * ---------------------------------------------------------------------------
 * PIXEL lives on your Stick. Two halves that feed each other:
 *
 *   PET MODE — feed, play, clean, heal, sleep. Care for it and it grows
 *              egg -> baby -> child -> adult; neglect it and it sickens.
 *
 *   ADVENTURE MODE (idle) — a mode you choose, independent of the cable.
 *              Hold the FRONT button to send it off fighting blobs; hold again
 *              to bring it home. ENERGY is its combat stamina: it fights until
 *              worn out, then rests. A well-fed, happy pet fights better. Good
 *              to leave running while it sits on your desk.
 *
 * CONTROLS
 *   Front tap  — move cursor / dismiss report
 *   Side  tap  — do the selected action
 *   Front hold — switch pet <-> adventure
 *   Side  hold — mute / unmute
 *   hold either on the gravestone — hatch a new egg
 *
 * BOARD: ESP32S3 Dev Module · OPI PSRAM · 8MB · 8M w/ spiffs · USB CDC on boot
 * LIBRARY: M5Unified (only)
 * ---------------------------------------------------------------------------
 */

#include <M5Unified.h>
#include <Preferences.h>
#include "esp_system.h"

// ------------------------------------------------------------------ logging --
// Serial at 115200. Prints the boot reset reason (a BROWNOUT shows up here) and
// a periodic status line, so power glitches and errors are visible during dev.
#define STICKPET_LOG 1
#if STICKPET_LOG
  #define LOGF(...) do{ Serial.printf("[%8lu] ",(unsigned long)millis()); Serial.printf(__VA_ARGS__); }while(0)
#else
  #define LOGF(...) do{}while(0)
#endif

// ------------------------------------------------------------ tuning knobs --
static const float HUNGER_DECAY = 8.0f;
static const float HAPPY_DECAY  = 6.0f;
static const float ENERGY_DECAY = 5.0f;
static const float ENERGY_REGEN = 45.0f;  // adventure: catch breath
static const float SLEEP_REGEN  = 80.0f;  // pet mode: a nap fills fast and visibly

static const float AGE_HATCH  = 0.5f;
static const float AGE_BABY   = 12.0f;
static const float AGE_CHILD  = 45.0f;

static const uint32_t SAVE_EVERY_MS = 30000;
static const uint32_t FRAME_MS      = 45;

// adventure
static const uint32_t ENCOUNTER_MS   = 5000;  // time between fights
static const uint32_t CLASH_MS       = 1000;  // when in a fight the blow lands
static const int      RETREAT_ENERGY = 12;    // energy %: pet rests instead of fighting
static const float    FIGHT_ENERGY   = 6.0f;  // energy spent per fight
static const float    ADV_CARE_SCALE  = 0.5f;  // care decays slower while away

static const uint8_t SPK_MAG = 8;
static const uint8_t SPK_VOL = 200;

// ---------------------------------------------------------------- pet state --
// stage: 0 egg, 1 baby, 2 child, 3 adult, 4 dead
struct PetState {
  float hunger, happy, energy, health;
  float ageMin;
  uint8_t stage;
  bool  asleep, sick;
  uint8_t poop;
  float sickTimer, poopTimer;
  uint16_t level;
  float xp;
  uint32_t gold;
  char name[12];
};
static PetState pet;

Preferences prefs;
static M5Canvas canvas(&M5.Display);

static uint32_t lastFrame = 0, lastSave = 0, lastTickMs = 0;
static int  cursor = 0;
static const int N_ACTIONS = 5;

// views
#define VIEW_HOME 0
#define VIEW_ADV  1
#define VIEW_RPT  2
static int view = VIEW_HOME;
static bool muted = false;


// adventure session
static uint32_t lastEncounter = 0, encStart = 0;
static bool  encActive = false;
static int   encBlobLvl = 1;
static bool  encWin = false;
static int   encXP = 0, encGold = 0;
static int   advKills = 0, advXP = 0, advGold = 0, advLevels = 0, advStartBatt = 0;
static bool  retreating = false;
static bool  encBoss = false;
static int   encBossType = 0;   // 0 crab, 1 spider, 2 deer
static int   cheatBoss = -1;    // dev: force next encounter to this boss type
static uint32_t fightCount = 0;

// animation
static uint32_t blinkUntil = 0, nextBlink = 0;
static uint32_t fxUntil = 0; static int fxKind = 0;

#define MOOD_DEAD 0
#define MOOD_SLEEP 1
#define MOOD_SICK 2
#define MOOD_HUNGRY 3
#define MOOD_SAD 4
#define MOOD_HAPPY 5
#define MOOD_OK 6

static uint16_t C_BG,C_BG2,C_INK,C_DIM,C_BODY,C_BELLY,C_EYE,C_CHEEK,
                C_GOOD,C_WARN,C_CRIT,C_ACC,C_POOP,C_FOE,C_XP,C_GOLD;

// =========================================================================
//  sound
// =========================================================================
static uint32_t lastToneEnd = 0;
static void spkPrimeIfCold(){ if(muted)return; if(millis()-lastToneEnd<350)return;
  M5.Speaker.setVolume(1); M5.Speaker.tone(4000,40); delay(60);
  while(M5.Speaker.isPlaying())delay(3); M5.Speaker.setVolume(SPK_VOL); }
static void note(uint16_t hz,uint16_t ms){ if(muted)return; M5.Speaker.tone(hz,ms); delay(ms);
  while(M5.Speaker.isPlaying())delay(2); lastToneEnd=millis(); }

static void sfxHatch(){ spkPrimeIfCold(); note(523,120);note(659,120);note(784,180);}
static void sfxEat()  { spkPrimeIfCold(); note(660,90); note(880,120);}
static void sfxPlay() { spkPrimeIfCold(); note(784,90); note(988,90);note(1175,140);}
static void sfxClean(){ spkPrimeIfCold(); note(1200,70);note(900,70);note(1400,90);}
static void sfxHeal() { spkPrimeIfCold(); note(880,110);note(1046,110);note(1318,160);}
static void sfxNope() { spkPrimeIfCold(); note(300,140);note(220,180);}
static void sfxSleep(){ spkPrimeIfCold(); note(500,140);note(380,200);}
static void sfxDie()  { spkPrimeIfCold(); note(440,180);note(349,200);note(262,320);}
static void sfxBoot() { spkPrimeIfCold(); note(660,110);note(990,110);note(1320,170);}
static void sfxHit()  { spkPrimeIfCold(); note(180,60); note(140,80);}
static void sfxWin()  { spkPrimeIfCold(); note(880,70); note(1175,110);}
static void sfxLevel(){ spkPrimeIfCold(); note(784,90);note(988,90);note(1175,90);note(1568,200);}
static void sfxDepart(){spkPrimeIfCold(); note(660,90);note(880,90);note(660,120);}
static void sfxBoss()  {spkPrimeIfCold(); note(200,160);note(160,200);note(220,160);note(150,240);}

// =========================================================================
//  persistence
// =========================================================================
static void saveState(){
  prefs.putFloat("hu",pet.hunger); prefs.putFloat("ha",pet.happy);
  prefs.putFloat("en",pet.energy); prefs.putFloat("he",pet.health);
  prefs.putFloat("ag",pet.ageMin); prefs.putUChar("st",pet.stage);
  prefs.putBool("as",pet.asleep);  prefs.putBool("si",pet.sick);
  prefs.putUChar("po",pet.poop);
  prefs.putUShort("lv",pet.level); prefs.putFloat("xp",pet.xp);
  prefs.putULong("gd",pet.gold);   prefs.putString("nm",pet.name);
  prefs.putBool("mu",muted);
  prefs.putULong("fc",fightCount);
  lastSave=millis();
}
static void hatchFresh(){
  pet.hunger=80;pet.happy=80;pet.energy=90;pet.health=100;
  pet.ageMin=0;pet.stage=0;pet.asleep=false;pet.sick=false;
  pet.poop=0;pet.sickTimer=0;pet.poopTimer=0;
  pet.level=1;pet.xp=0;pet.gold=0;
  strncpy(pet.name,"PIXEL",sizeof(pet.name));
  saveState();
}
static void loadState(){
  prefs.begin("stickpet",false);
  if(!prefs.isKey("st")){ hatchFresh(); return; }
  pet.hunger=prefs.getFloat("hu",80); pet.happy=prefs.getFloat("ha",80);
  pet.energy=prefs.getFloat("en",90); pet.health=prefs.getFloat("he",100);
  pet.ageMin=prefs.getFloat("ag",0);  pet.stage=prefs.getUChar("st",0);
  pet.asleep=prefs.getBool("as",false); pet.sick=prefs.getBool("si",false);
  pet.poop=prefs.getUChar("po",0);
  pet.level=prefs.getUShort("lv",1); pet.xp=prefs.getFloat("xp",0);
  pet.gold=prefs.getULong("gd",0);
  muted=prefs.getBool("mu",false);
  fightCount=prefs.getULong("fc",0);
  String nm=prefs.getString("nm","PIXEL");
  strncpy(pet.name,nm.c_str(),sizeof(pet.name)-1); pet.name[sizeof(pet.name)-1]=0;
  pet.sickTimer=0; pet.poopTimer=0;
}

// =========================================================================
//  sim
// =========================================================================
static float clampf(float v){ return v<0?0:(v>100?100:v); }
static float xpNeeded(){ return 40 + pet.level*20; }

static int currentMood(){
  if(pet.stage==4)return MOOD_DEAD;
  if(pet.asleep)return MOOD_SLEEP;
  if(pet.sick)return MOOD_SICK;
  if(pet.hunger<25)return MOOD_HUNGRY;
  if(pet.happy<25)return MOOD_SAD;
  if(pet.hunger>60&&pet.happy>60)return MOOD_HAPPY;
  return MOOD_OK;
}

static void grantXP(int amt){
  pet.xp += amt;
  while(pet.xp >= xpNeeded()){ pet.xp -= xpNeeded(); pet.level++; advLevels++; sfxLevel(); LOGF("LEVEL UP -> %d\n", pet.level); }
}

static void advance(float dtMin, bool adventuring){
  if(pet.stage==4)return;
  pet.ageMin+=dtMin;
  if(pet.stage==0&&pet.ageMin>=AGE_HATCH){ pet.stage=1; sfxHatch(); LOGF("hatched\n"); }
  else if(pet.stage==1&&pet.ageMin>=AGE_BABY){ pet.stage=2; sfxLevel(); LOGF("evolve -> child\n"); }
  else if(pet.stage==2&&pet.ageMin>=AGE_CHILD){ pet.stage=3; sfxLevel(); LOGF("evolve -> adult\n"); }
  if(pet.stage==0)return;

  float scale = adventuring ? ADV_CARE_SCALE : 1.0f;
  if(pet.asleep && !adventuring){
    pet.energy=clampf(pet.energy+SLEEP_REGEN*dtMin);
    pet.hunger=clampf(pet.hunger-HUNGER_DECAY*0.4f*dtMin);
    pet.happy =clampf(pet.happy -HAPPY_DECAY*0.2f*dtMin);
    if(pet.energy>=100)pet.asleep=false;
  } else {
    pet.hunger=clampf(pet.hunger-HUNGER_DECAY*scale*dtMin);
    pet.happy =clampf(pet.happy -HAPPY_DECAY *scale*dtMin);
    if(!adventuring) pet.energy=clampf(pet.energy-ENERGY_DECAY*dtMin);
  }

  if(!adventuring){
    pet.poopTimer+=dtMin;
    if(pet.poopTimer>1.5f&&pet.poop<3&&random(100)<22){pet.poop++;pet.poopTimer=0;}
  }

  bool neglected=(pet.hunger<=0||pet.happy<=0||pet.poop>=3);
  if(neglected)pet.sickTimer+=dtMin; else pet.sickTimer=clampf(pet.sickTimer-dtMin*0.5f);
  if(pet.sickTimer>1.0f)pet.sick=true;
  if(pet.sick){
    pet.health=clampf(pet.health-10.0f*dtMin);
    if(pet.health<=0){pet.stage=4;pet.asleep=false;sfxDie();saveState();LOGF("DIED at age %dm lv%d\n",(int)pet.ageMin,pet.level);}
  } else if(pet.health<100) pet.health=clampf(pet.health+4.0f*dtMin);
}

// combat power drawn from level + how well cared-for it is
static int petPower(){
  float care=(pet.hunger+pet.happy+pet.energy)/30.0f;  // 0..10
  return pet.level*3 + (int)care + (int)random(0,4);
}

static void startEncounter(){
  encActive=true; encStart=millis();
  fightCount++;
  encBoss = (fightCount % 10 == 0) || (cheatBoss>=0);   // every 10th enemy is a boss
  if(encBoss){
    encBossType = (cheatBoss>=0) ? cheatBoss : (((fightCount/10) - 1) % 3);   // crab -> spider -> deer
    cheatBoss = -1;
    encBlobLvl = pet.level + 2;                       // tougher than the pet
    int foe = encBlobLvl*4 + (int)random(0,6);        // and hits harder
    encWin = petPower() >= foe;                       // care/level really matter here
    if(encWin){ encXP = (8 + encBlobLvl*4)*3; encGold = (1 + encBlobLvl)*3 + (int)random(0,6); }
    else { encXP=0; encGold=0; }
    sfxBoss();
  } else {
    encBlobLvl = pet.level + (int)random(-1,3); if(encBlobLvl<1)encBlobLvl=1;
    int foe = encBlobLvl*3 + (int)random(0,5);
    encWin = petPower() >= foe;
    if(encWin){ encXP = 8 + encBlobLvl*4; encGold = 1 + (int)random(0,4) + encBlobLvl; }
    else { encXP=0; encGold=0; }
  }
}

// apply the encounter result at the "clash" moment
static bool encApplied=false;
static void resolveEncounter(){
  if(encWin){
    advKills++; advXP+=encXP; advGold+=encGold;
    pet.gold+=encGold; grantXP(encXP); sfxWin();
  } else {
    pet.happy=clampf(pet.happy-4);
    // a beating only draws blood if the pet is in poor shape
    bool frail=(pet.hunger<30 || pet.happy<30 || pet.sick);
    if(frail) pet.health=clampf(pet.health-(8+random(0,8)));
    sfxHit();
  }
  LOGF("%sfight Lv%d: %-4s +%dxp +%dg  en=%d he=%d\n", encBoss?"BOSS ":"", encBlobLvl, encWin?"WIN":"miss", encXP, encGold, (int)pet.energy, (int)pet.health);
}

// =========================================================================
//  drawing — shared creature
// =========================================================================
static void drawCreature(int cx,int cy,int mood,uint32_t now,bool battleStance){
  float scale=pet.stage==1?0.7f:(pet.stage==2?0.85f:1.0f);
  int rx=(int)(30*scale), ry=(int)(27*scale);
  int bounce=0;
  if(mood!=MOOD_SLEEP&&mood!=MOOD_DEAD) bounce=(int)(sinf(now/260.0f)*3.0f);
  cy+=bounce;
  uint16_t body=pet.sick?canvas.color565(120,150,90):C_BODY;

  if(pet.stage==0){
    int wob=(int)(sinf(now/180.0f)*2.0f);
    canvas.fillEllipse(cx+wob,cy,20,26,C_BELLY);
    canvas.drawEllipse(cx+wob,cy,20,26,C_INK);
    canvas.fillCircle(cx+wob-7,cy-3,3,C_ACC);
    canvas.fillCircle(cx+wob+5,cy+6,3,C_ACC);
    return;
  }
  if(mood==MOOD_DEAD){
    canvas.fillRoundRect(cx-24,cy-18,48,42,8,C_DIM);
    canvas.setTextColor(C_BG);canvas.setTextSize(2);
    canvas.setCursor(cx-14,cy-10);canvas.print("RIP");
    return;
  }
  canvas.fillEllipse(cx-rx/2,cy+ry-2,7,4,body);
  canvas.fillEllipse(cx+rx/2,cy+ry-2,7,4,body);
  canvas.drawLine(cx,cy-ry,cx,cy-ry-9,C_INK);
  canvas.fillCircle(cx,cy-ry-11,3,mood==MOOD_HAPPY?C_GOOD:C_ACC);
  canvas.fillEllipse(cx,cy,rx,ry,body);
  canvas.fillEllipse(cx,cy+3,(int)(rx*0.6f),(int)(ry*0.6f),C_BELLY);

  int eyeDX=(int)(11*scale), eyeY=cy-3;
  bool blinking=now<blinkUntil;
  if(battleStance){ // determined eyes
    canvas.fillCircle(cx-eyeDX,eyeY,5,C_EYE);
    canvas.fillCircle(cx+eyeDX,eyeY,5,C_EYE);
    canvas.fillCircle(cx-eyeDX+1,eyeY,3,C_INK);
    canvas.fillCircle(cx+eyeDX+1,eyeY,3,C_INK);
    canvas.drawLine(cx-eyeDX-5,eyeY-6,cx-eyeDX+3,eyeY-3,C_INK); // brow
    canvas.drawLine(cx+eyeDX+5,eyeY-6,cx+eyeDX-3,eyeY-3,C_INK);
  } else if(mood==MOOD_SLEEP||blinking){
    canvas.drawLine(cx-eyeDX-4,eyeY,cx-eyeDX+4,eyeY,C_INK);
    canvas.drawLine(cx+eyeDX-4,eyeY,cx+eyeDX+4,eyeY,C_INK);
  } else if(mood==MOOD_SICK){
    canvas.drawLine(cx-eyeDX-4,eyeY-4,cx-eyeDX+4,eyeY+4,C_INK);
    canvas.drawLine(cx-eyeDX-4,eyeY+4,cx-eyeDX+4,eyeY-4,C_INK);
    canvas.drawLine(cx+eyeDX-4,eyeY-4,cx+eyeDX+4,eyeY+4,C_INK);
    canvas.drawLine(cx+eyeDX-4,eyeY+4,cx+eyeDX+4,eyeY-4,C_INK);
  } else {
    canvas.fillCircle(cx-eyeDX,eyeY,6,C_EYE);
    canvas.fillCircle(cx+eyeDX,eyeY,6,C_EYE);
    int pdy=(mood==MOOD_HUNGRY||mood==MOOD_SAD)?2:0;
    canvas.fillCircle(cx-eyeDX,eyeY+pdy,3,C_INK);
    canvas.fillCircle(cx+eyeDX,eyeY+pdy,3,C_INK);
  }
  if(mood==MOOD_HAPPY){
    canvas.fillCircle(cx-eyeDX-6,eyeY+8,3,C_CHEEK);
    canvas.fillCircle(cx+eyeDX+6,eyeY+8,3,C_CHEEK);
  }
  int my=cy+11;
  if(battleStance) canvas.fillArc(cx,my-2,7,9,20,160,C_INK);
  else if(mood==MOOD_HAPPY) canvas.fillArc(cx,my-4,7,9,20,160,C_INK);
  else if(mood==MOOD_OK) canvas.drawLine(cx-5,my,cx+5,my,C_INK);
  else if(mood==MOOD_SAD||mood==MOOD_SICK) canvas.fillArc(cx,my+8,7,9,200,340,C_INK);
  else if(mood==MOOD_HUNGRY) canvas.fillCircle(cx,my+1,4,C_INK);

  if(mood==MOOD_SLEEP){
    canvas.setTextColor(C_DIM);canvas.setTextSize(2);
    canvas.setCursor(cx+rx,cy-ry-8);canvas.print("Z");
  }
  canvas.setTextSize(1);   // reset: the 'Z' bumped size to 2, callers draw text next
}

static void drawBlob(int cx,int cy,uint32_t now,int lvl,bool popping,float pop){
  if(popping){
    for(int i=0;i<8;i++){
      float a=i*0.785f; int r=(int)(6+pop*18);
      int px=cx+(int)(cosf(a)*r), py=cy+(int)(sinf(a)*r);
      canvas.fillCircle(px,py,(int)(3*(1-pop))+1,C_FOE);
    }
    return;
  }
  int wob=(int)(sinf(now/150.0f)*2.0f);
  canvas.fillEllipse(cx,cy+wob,16,14,C_FOE);
  canvas.fillCircle(cx-5,cy-2+wob,3,C_EYE);
  canvas.fillCircle(cx+5,cy-2+wob,3,C_EYE);
  canvas.fillCircle(cx-5,cy-1+wob,1,C_INK);
  canvas.fillCircle(cx+5,cy-1+wob,1,C_INK);
  canvas.drawLine(cx-8,cy-8+wob,cx-2,cy-5+wob,C_INK);
  canvas.drawLine(cx+8,cy-8+wob,cx+2,cy-5+wob,C_INK);
  canvas.setTextColor(C_DIM);canvas.setTextSize(1);
  canvas.setCursor(cx-6,cy+16);canvas.printf("Lv%d",lvl);
}

// Three rotating bosses, drawn from primitives, sized to fit y ~40..88.
static void drawBoss(int cx,int cy,uint32_t now,int type,int lvl,bool popping,float pop){
  uint16_t col = type==0 ? canvas.color565(225,95,60)     // crab  - red-orange
               : type==1 ? canvas.color565(120,70,160)    // spider- purple
                         : canvas.color565(150,110,70);    // deer  - brown
  const char* nm = type==0 ? "CRAB" : type==1 ? "SPIDER" : "DEER";

  if(popping){
    for(int i=0;i<14;i++){ float a=i*(6.2832f/14); int r=(int)(9+pop*26);
      canvas.fillCircle(cx+(int)(cosf(a)*r),cy+(int)(sinf(a)*r),(int)(4*(1-pop))+1,col); }
    return;
  }
  int wob=(int)(sinf(now/160.0f)*2.0f); int yy=cy+wob;

  if(type==0){                       // ---- CRAB ----
    canvas.fillEllipse(cx,yy,23,12,col);
    for(int i=-2;i<=2;i++){ if(!i)continue; canvas.drawLine(cx+i*7,yy+7,cx+i*9,yy+15,col); }
    canvas.fillCircle(cx-24,yy+1,6,col); canvas.fillCircle(cx+24,yy+1,6,col);
    canvas.fillTriangle(cx-30,yy-3,cx-22,yy-5,cx-26,yy+1,col);
    canvas.fillTriangle(cx+30,yy-3,cx+22,yy-5,cx+26,yy+1,col);
    canvas.drawLine(cx-6,yy-11,cx-6,yy-18,col); canvas.drawLine(cx+6,yy-11,cx+6,yy-18,col);
    canvas.fillCircle(cx-6,yy-20,3,C_EYE); canvas.fillCircle(cx+6,yy-20,3,C_EYE);
    canvas.fillCircle(cx-6,yy-20,1,C_INK); canvas.fillCircle(cx+6,yy-20,1,C_INK);
  } else if(type==1){                // ---- SPIDER ----
    for(int i=0;i<4;i++){ int ly=yy-6+i*5;
      canvas.drawLine(cx-7,yy-1,cx-22,ly-4,col);
      canvas.drawLine(cx+7,yy-1,cx+22,ly-4,col); }
    canvas.fillCircle(cx,yy+4,12,col);
    canvas.fillCircle(cx,yy-8,8,col);
    canvas.fillCircle(cx-3,yy-10,2,C_EYE); canvas.fillCircle(cx+3,yy-10,2,C_EYE);
    canvas.fillCircle(cx-6,yy-7,1,C_EYE);  canvas.fillCircle(cx+6,yy-7,1,C_EYE);
    canvas.fillCircle(cx-3,yy-10,1,C_INK); canvas.fillCircle(cx+3,yy-10,1,C_INK);
  } else {                           // ---- DEER (the scary one) ----
    canvas.fillEllipse(cx-4,yy+6,15,10,col);
    canvas.fillRect(cx+5,yy-8,6,14,col);
    canvas.fillEllipse(cx+11,yy-12,9,7,col);
    canvas.fillTriangle(cx+5,yy-16,cx+8,yy-21,cx+10,yy-14,col);
    canvas.fillTriangle(cx+17,yy-16,cx+14,yy-21,cx+12,yy-14,col);
    uint16_t ant=canvas.color565(235,225,205);
    canvas.drawLine(cx+8,yy-18,cx+6,yy-26,ant);  canvas.drawLine(cx+6,yy-26,cx+2,yy-28,ant);  canvas.drawLine(cx+6,yy-26,cx+8,yy-30,ant);
    canvas.drawLine(cx+15,yy-18,cx+17,yy-26,ant);canvas.drawLine(cx+17,yy-26,cx+21,yy-28,ant);canvas.drawLine(cx+17,yy-26,cx+15,yy-30,ant);
    canvas.drawLine(cx-10,yy+13,cx-10,yy+19,col); canvas.drawLine(cx+2,yy+13,cx+2,yy+19,col);
    canvas.fillCircle(cx+13,yy-12,2,C_CRIT);   // single glowing eye
  }

  canvas.setTextColor(C_CRIT);canvas.setTextSize(1);
  char lbl[16]; snprintf(lbl,sizeof(lbl),"%s L%d",nm,lvl);
  int tw=strlen(lbl)*6; canvas.setCursor(cx-tw/2,cy+22); canvas.print(lbl);
}

// dispatch: bosses use their own art, everything else the basic blob
static void drawFoe(int cx,int cy,uint32_t now,int lvl,bool popping,float pop){
  if(encBoss) drawBoss(cx,cy,now,encBossType,lvl,popping,pop);
  else        drawBlob(cx,cy,now,lvl,popping,pop);
}

static void statBar(int x,int y,int w,const char*label,float v,uint16_t col){
  canvas.setTextSize(1);canvas.setTextColor(C_DIM);
  canvas.setCursor(x,y-9);canvas.print(label);
  canvas.drawRoundRect(x,y,w,8,2,C_DIM);
  int fill=(int)((w-2)*(v/100.0f));
  if(fill>0)canvas.fillRoundRect(x+1,y+1,fill,6,2,col);
}
static void drawPoop(uint32_t now){
  for(int i=0;i<pet.poop;i++){
    int px=40+i*26,py=96;
    canvas.fillArc(px,py,0,7,180,360,C_POOP);
    canvas.fillArc(px,py-4,0,5,180,360,C_POOP);
    canvas.fillCircle(px,py-7,2,C_POOP);
  }
}
static void drawFx(uint32_t now,int cx,int cy){
  if(now>=fxUntil)return;
  float t=1.0f-(float)(fxUntil-now)/900.0f;
  if(fxKind==1){int fy=(int)(cy-40+t*46);canvas.fillCircle(cx,fy,5,C_WARN);}
  else if(fxKind==2){for(int i=0;i<3;i++){int hy=(int)(cy-t*40)-i*8,hx=cx+(i-1)*16;
    canvas.fillCircle(hx-2,hy,2,C_CHEEK);canvas.fillCircle(hx+2,hy,2,C_CHEEK);
    canvas.fillTriangle(hx-4,hy+1,hx+4,hy+1,hx,hy+6,C_CHEEK);}}
  else if(fxKind==3){for(int i=0;i<5;i++){int sx=30+i*40,sy=(int)(70-sinf(t*3.14159f+i)*20);
    canvas.drawLine(sx-3,sy,sx+3,sy,C_ACC);canvas.drawLine(sx,sy-3,sx,sy+3,C_ACC);}}
  else if(fxKind==4){for(int i=0;i<3;i++){int hy=(int)(cy-t*34)-i*6,hx=cx+(i-1)*18;
    canvas.fillRect(hx-1,hy-4,3,9,C_GOOD);canvas.fillRect(hx-4,hy-1,9,3,C_GOOD);}}
}
static const char* ACTS[N_ACTIONS]={"FEED","PLAY","REST","CLEAN","HEAL"};
static void drawActionBar(){
  int w=240/N_ACTIONS,y=120;
  for(int i=0;i<N_ACTIONS;i++){
    int x=i*w; bool sel=(i==cursor);
    if(sel)canvas.fillRoundRect(x+1,y,w-2,14,3,C_ACC);
    canvas.setTextColor(sel?C_BG:C_DIM);canvas.setTextSize(1);
    int tw=strlen(ACTS[i])*6; canvas.setCursor(x+(w-tw)/2,y+4);canvas.print(ACTS[i]);
  }
}
static void drawMuteIcon(int x,int y){
  uint16_t c = muted ? C_CRIT : C_DIM;
  canvas.fillRect(x, y+1, 3, 5, c);
  canvas.fillTriangle(x+3, y-1, x+3, y+7, x+8, y+3, c);
  if(muted){
    canvas.drawLine(x-1, y-2, x+10, y+8, C_CRIT);
  } else {
    canvas.drawArc(x+9, y+3, 3, 4, 300, 60, c);
  }
}

static void renderDead(uint32_t now){
  canvas.fillSprite(C_BG);
  canvas.setTextSize(1); canvas.setTextColor(C_DIM);
  canvas.setCursor(4,4); canvas.print(pet.name);
  canvas.setCursor(198,4); canvas.printf("Lv%d",pet.level);

  int cx=120, cy=56;
  canvas.fillRoundRect(cx-28,cy-24,56,48,10,C_DIM);
  canvas.setTextColor(C_BG); canvas.setTextSize(3);
  canvas.setCursor(cx-24,cy-11); canvas.print("RIP");

  char buf[40];
  snprintf(buf,sizeof(buf),"lived %dm - reached Lv%d",(int)pet.ageMin,pet.level);
  canvas.setTextSize(1); canvas.setTextColor(C_DIM);
  int tw=strlen(buf)*6; canvas.setCursor((240-tw)/2,96); canvas.print(buf);

  const char* h="hold a button to hatch a new egg";
  canvas.setTextColor(C_ACC);
  int hw=strlen(h)*6; canvas.setCursor((240-hw)/2,116); canvas.print(h);

  canvas.pushSprite(0,0);
}

static void renderHome(uint32_t now){
  int mood=currentMood();
  canvas.fillSprite(C_BG);
  canvas.fillRect(0,100,240,18,C_BG2);
  canvas.setTextSize(1);canvas.setTextColor(C_INK);
  canvas.setCursor(4,3);canvas.print(pet.name);
  const char* sn=pet.stage==0?"egg":pet.stage==1?"baby":pet.stage==2?"child":pet.stage==3?"adult":"--";
  canvas.setTextColor(C_DIM);canvas.setCursor(58,3);canvas.printf("%s Lv%d",sn,pet.level);
  int batt=M5.Power.getBatteryLevel();
  canvas.setCursor(150,3);canvas.printf("G:%lu",(unsigned long)pet.gold);
  drawMuteIcon(190,4);
  canvas.setTextColor(C_DIM);
  canvas.setCursor(206,3); if(batt>=0)canvas.printf("%d%%",batt);else canvas.print("--");
  canvas.drawFastHLine(0,13,240,C_BG2);

  int cx=120,cy=58;
  drawPoop(now); drawCreature(cx,cy,mood,now,false); drawFx(now,cx,cy);
  if(pet.stage>=1 && pet.stage<=3){
    canvas.setTextColor(C_DIM);canvas.setTextSize(1);
    canvas.setCursor(66,90);canvas.print("hold A: adventure");
  }
  if(pet.sick&&pet.stage!=4){canvas.setTextSize(1);canvas.setTextColor(C_CRIT);canvas.setCursor(4,16);canvas.print("SICK!");}

  if(pet.stage!=4){
    statBar(10,108,66,"FOOD",pet.hunger,pet.hunger<25?C_CRIT:pet.hunger<50?C_WARN:C_GOOD);
    statBar(88,108,66,"FUN", pet.happy, pet.happy <25?C_CRIT:pet.happy <50?C_WARN:C_GOOD);
    statBar(166,108,66,"REST",pet.energy,pet.energy<25?C_CRIT:pet.energy<50?C_WARN:C_GOOD);
  }
  drawActionBar();
  canvas.pushSprite(0,0);
}

// =========================================================================
//  ADVENTURE view (unplugged)
// =========================================================================
static void renderAdventure(uint32_t now){
  canvas.fillSprite(canvas.color565(14,18,30));
  // ground
  canvas.fillRect(0,86,240,20,canvas.color565(30,40,34));
  // stars
  for(int i=0;i<12;i++){int sx=(i*53+13)%240,sy=(i*29+7)%70;canvas.drawPixel(sx,sy,C_DIM);}

  canvas.setTextSize(1);
  if(encActive && encBoss){ canvas.setTextColor(C_CRIT); canvas.setCursor(4,3);
    const char* bn=encBossType==0?"!! CRAB BOSS !!":encBossType==1?"!! SPIDER BOSS !!":"!! DEER BOSS !!";
    canvas.print(bn); }
  else { canvas.setTextColor(C_ACC); canvas.setCursor(4,3); canvas.print("* ADVENTURING *"); }
  canvas.setTextColor(C_DIM);canvas.setCursor(150,3);canvas.printf("Lv%d",pet.level);
  drawMuteIcon(218,4);

  int en=(int)pet.energy;
  canvas.setTextColor(C_DIM);canvas.setCursor(4,15);canvas.print("STAMINA");
  canvas.drawRoundRect(4,25,232,12,3,C_DIM);
  uint16_t sc=en<RETREAT_ENERGY?C_CRIT:en<40?C_WARN:C_GOOD;
  int fw=(int)((230)*(en/100.0f)); if(fw>0)canvas.fillRoundRect(5,26,fw,10,3,sc);
  canvas.setTextColor(C_INK);canvas.setCursor(112,27);canvas.printf("%d",en);
  int batt=M5.Power.getBatteryLevel();
  canvas.setTextColor(C_DIM);canvas.setCursor(196,15); if(batt>=0)canvas.printf("bat%d",batt);

  int mood=currentMood();
  int petX=70, foeX=170, cy=68;

  if(retreating){
    drawCreature(petX,cy,MOOD_SLEEP,now,false);
    canvas.setTextSize(1);
    canvas.setTextColor(C_WARN);
    const char* m="worn out - catching breath";
    int mw=strlen(m)*6; canvas.setCursor((240-mw)/2,98); canvas.print(m);
    char t[40]; snprintf(t,sizeof(t),"kills %d  +%d XP  +%d G",advKills,advXP,advGold);
    canvas.setTextColor(C_DIM);
    int tw=strlen(t)*6; canvas.setCursor((240-tw)/2,118); canvas.print(t);
    canvas.pushSprite(0,0); return;
  }

  bool stance = encActive;
  // encounter animation
  if(encActive){
    float t=(now-encStart)/2200.0f;           // 0..1 over the fight
    if(t>1.0f){ encActive=false; }
    int lunge=(int)(sinf(fminf(t,1.0f)*3.14159f)*22); // meet in the middle
    int px=petX+lunge, fx=foeX-lunge;
    bool clash = (t>0.42f && t<0.60f);
    bool post  = (t>=0.60f);

    if(clash){ canvas.fillCircle(120,cy,14,C_WARN); }  // impact flash (reward applied in loop)
    drawCreature(px,cy,mood,now,true);
    if(encWin && post){
      float pop=(t-0.60f)/0.40f; drawFoe(fx,cy,now,encBlobLvl,true,pop);
      canvas.setTextColor(C_XP);canvas.setCursor(fx-10,cy-26);canvas.printf("+%dXP",encXP);
      canvas.setTextColor(C_GOLD);canvas.setCursor(fx-6,cy-16);canvas.printf("+%dG",encGold);
    } else if(!encWin && post){
      drawFoe(fx,cy,now,encBlobLvl,false,0);
      canvas.setTextColor(C_CRIT);canvas.setCursor(px-12,cy-24);canvas.print("miss!");
    } else {
      drawFoe(fx,cy,now,encBlobLvl,false,0);
    }
  } else {
    drawCreature(petX,cy,mood,now,false);
    canvas.setTextColor(C_DIM);canvas.setCursor(150,60);canvas.print("...");
  }
  {
    canvas.setTextSize(1);
    if(pet.health<40){
      canvas.setTextColor(C_CRIT);
      const char* d="! hurt - bring it home !";
      int dw=strlen(d)*6; canvas.setCursor((240-dw)/2,44); canvas.print(d);
    } else {
      canvas.setTextColor(C_DIM);
      canvas.setCursor(60,44);canvas.print("hold A: back to pet");
    }
  }

  // xp bar
  canvas.setTextColor(C_DIM);canvas.setCursor(4,108);
  canvas.printf("XP %d/%d",(int)pet.xp,(int)xpNeeded());
  canvas.drawRoundRect(4,116,150,7,2,C_DIM);
  int xw=(int)(148*(pet.xp/xpNeeded())); if(xw>0)canvas.fillRoundRect(5,117,xw,5,2,C_XP);
  canvas.setTextColor(C_GOLD);canvas.setCursor(165,116);canvas.printf("kills %d",advKills);

  canvas.pushSprite(0,0);
}

// =========================================================================
//  REPORT view (on replug)
// =========================================================================
static void renderReport(uint32_t now){
  canvas.fillSprite(C_BG);
  canvas.setTextSize(2);canvas.setTextColor(C_ACC);
  canvas.setCursor(40,8);canvas.print("WELCOME BACK");
  canvas.drawFastHLine(0,30,240,C_BG2);

  drawCreature(40,70,MOOD_HAPPY,now,false);

  canvas.setTextSize(1);
  int x=90,y=40;
  canvas.setTextColor(C_DIM);canvas.setCursor(x,y);   canvas.print("While you were away:");
  canvas.setTextColor(C_INK);
  canvas.setTextColor(C_GOOD);canvas.setCursor(x,y+16);canvas.printf("blobs beaten : %d",advKills);
  canvas.setTextColor(C_XP);  canvas.setCursor(x,y+28);canvas.printf("XP earned    : %d",advXP);
  canvas.setTextColor(C_GOLD);canvas.setCursor(x,y+40);canvas.printf("gold found   : %d",advGold);
  if(advLevels>0){canvas.setTextColor(C_ACC);canvas.setCursor(x,y+52);canvas.printf("LEVEL UP x%d -> Lv%d",advLevels,pet.level);}

  canvas.setTextColor(C_DIM);canvas.setTextSize(1);
  canvas.setCursor(50,122);canvas.print("press a button to continue");
  canvas.pushSprite(0,0);
}

// =========================================================================
//  actions
// =========================================================================
static void doFeed(){ if(pet.stage==0||pet.stage==4){sfxNope();return;}
  pet.asleep=false;                       // any action wakes it
  if(pet.hunger>96){sfxNope();return;}
  pet.hunger=clampf(pet.hunger+28);pet.energy=clampf(pet.energy+4);
  fxKind=1;fxUntil=millis()+900;sfxEat();}
static void doPlay(){ if(pet.stage==0||pet.stage==4){sfxNope();return;}
  pet.asleep=false;
  if(pet.energy<12){sfxNope();return;}
  pet.happy=clampf(pet.happy+26);pet.energy=clampf(pet.energy-14);pet.hunger=clampf(pet.hunger-5);
  fxKind=2;fxUntil=millis()+1000;sfxPlay();}
static void doRest(){ if(pet.stage==0||pet.stage==4){sfxNope();return;}
  pet.asleep=!pet.asleep;sfxSleep();}
static void doClean(){ if(pet.stage==4||pet.poop==0){sfxNope();return;}
  pet.asleep=false;
  pet.poop=0;pet.happy=clampf(pet.happy+6);fxKind=3;fxUntil=millis()+800;sfxClean();}
static void doHeal(){ if(pet.stage==4||(!pet.sick&&pet.health>96)){sfxNope();return;}
  pet.asleep=false;
  pet.sick=false;pet.sickTimer=0;pet.health=clampf(pet.health+45);
  fxKind=4;fxUntil=millis()+900;sfxHeal();}
static void doAction(int a){
  switch(a){case 0:doFeed();break;case 1:doPlay();break;case 2:doRest();break;
            case 3:doClean();break;case 4:doHeal();break;} saveState();
}

// =========================================================================
static void initPalette(){
  C_BG=canvas.color565(20,26,40);   C_BG2=canvas.color565(34,42,60);
  C_INK=canvas.color565(20,20,28);  C_DIM=canvas.color565(150,160,180);
  C_BODY=canvas.color565(120,200,235);C_BELLY=canvas.color565(225,245,250);
  C_EYE=canvas.color565(255,255,255);C_CHEEK=canvas.color565(255,130,150);
  C_GOOD=canvas.color565(90,210,120);C_WARN=canvas.color565(240,190,70);
  C_CRIT=canvas.color565(235,90,80); C_ACC=canvas.color565(150,130,240);
  C_POOP=canvas.color565(140,96,60); C_FOE=canvas.color565(180,110,220);
  C_XP=canvas.color565(120,210,255); C_GOLD=canvas.color565(245,205,90);
}

static void logResetReason(){
#if STICKPET_LOG
  esp_reset_reason_t r=esp_reset_reason(); const char* w;
  switch(r){
    case ESP_RST_POWERON: w="power-on"; break;
    case ESP_RST_SW:      w="software"; break;
    case ESP_RST_PANIC:   w="PANIC/crash"; break;
    case ESP_RST_TASK_WDT:w="task-watchdog"; break;
    case ESP_RST_INT_WDT: w="int-watchdog"; break;
    case ESP_RST_BROWNOUT:w="BROWNOUT (power dipped)"; break;
    case ESP_RST_DEEPSLEEP:w="wake-deepsleep"; break;
    case ESP_RST_EXT:     w="external"; break;
    default:              w="other"; break;
  }
  Serial.printf("\n[boot] reset reason: %s\n", w);
#endif
}

void setup(){
  auto cfg=M5.config(); cfg.internal_spk=true; cfg.internal_mic=false; M5.begin(cfg);
  M5.Display.setRotation(1); M5.Display.setBrightness(90);
  M5.Speaker.end();
  { auto s=M5.Speaker.config(); s.magnification=SPK_MAG; M5.Speaker.config(s); }
  M5.Speaker.begin(); M5.Speaker.setVolume(SPK_VOL);
  canvas.setColorDepth(16); canvas.createSprite(240,135); initPalette();
  randomSeed(esp_random());
  Serial.begin(115200); delay(200);
  logResetReason();
  loadState();
  LOGF("StickPet boot: stage=%d lv=%d hp=%d muted=%d\n",pet.stage,pet.level,(int)pet.health,(int)muted);
  sfxBoot();
  lastTickMs=millis(); nextBlink=millis()+2000+random(2000);
  view=VIEW_HOME;
}

void loop(){
  M5.update();
  uint32_t now=millis();

  // side-button hold: mute / unmute, in any view
  if(M5.BtnB.wasHold()){ muted=!muted; prefs.putBool("mu",muted); LOGF("mute=%d\n",(int)muted); }

#if STICKPET_LOG
  // ---- dev cheats over serial (dev builds only) ----
  while(Serial.available()){
    int c=Serial.read();
    switch(c){
      case 'k': pet.health=0; pet.sick=true; pet.stage=4; view=VIEW_HOME; sfxDie();
                LOGF("cheat: killed\n"); saveState(); break;
      case 's': pet.sick=true; pet.sickTimer=2; pet.health=25; LOGF("cheat: sick\n"); break;
      case 'f': pet.hunger=pet.happy=pet.energy=pet.health=100; pet.sick=false;
                pet.sickTimer=0; pet.poop=0; if(pet.stage==0)pet.stage=1;
                LOGF("cheat: full stats\n"); saveState(); break;
      case 'x': grantXP((int)xpNeeded()); LOGF("cheat: +level -> %d\n",pet.level); saveState(); break;
      case 'e': pet.ageMin+=AGE_CHILD; LOGF("cheat: age+ -> stage advances\n"); break;
      case 'p': if(pet.poop<3)pet.poop++; LOGF("cheat: poop=%d\n",pet.poop); break;
      case 'n': hatchFresh(); view=VIEW_HOME; LOGF("cheat: new egg\n"); break;
      case 'b': { static int bt=-1; bt=(bt+1)%3; cheatBoss=bt;
                  if(pet.stage>=1&&pet.stage<=3){ view=VIEW_ADV; encActive=false;
                    advKills=advXP=advGold=advLevels=0; lastEncounter=millis()-ENCOUNTER_MS-100; retreating=false; }
                  LOGF("cheat: next boss = %s\n", bt==0?"crab":bt==1?"spider":"deer"); } break;
      case '?': LOGF("cheats: k=kill s=sick f=fill x=+level e=age+ p=poop n=new egg b=spawn boss(cycles)\n"); break;
      default: break;
    }
  }
#endif

  if(pet.stage==4){
    // gravestone: hold either button to hatch a new egg
    if(M5.BtnA.pressedFor(1200)||M5.BtnB.pressedFor(1200)){ hatchFresh(); sfxHatch(); view=VIEW_HOME; LOGF("hatched anew\n"); }
  } else {
    // front-button hold: toggle pet <-> adventure (a chosen mode, not the cable)
    if(view!=VIEW_RPT && M5.BtnA.wasHold()){
      if(view==VIEW_ADV){
        if(advKills>0||advXP>0||advLevels>0){ view=VIEW_RPT; sfxWin(); }
        else view=VIEW_HOME;
        encActive=false; retreating=false; saveState();
        LOGF("exit ADV: kills=%d xp=%d gold=%d lvUp=%d\n",advKills,advXP,advGold,advLevels);
      } else if(pet.stage>=1 && pet.stage<=3){
        view=VIEW_ADV; advKills=advXP=advGold=advLevels=0;
        lastEncounter=now-ENCOUNTER_MS+800; encActive=false; retreating=false;
        sfxDepart(); LOGF("enter ADV\n");
      }
    }
    // short-press input per view
    if(view==VIEW_RPT){
      if(M5.BtnA.wasClicked()||M5.BtnB.wasClicked()) view=VIEW_HOME;
    } else if(view==VIEW_HOME){
      if(M5.BtnA.wasClicked()) cursor=(cursor+1)%N_ACTIONS;   // navigation is silent
      if(M5.BtnB.wasClicked()) doAction(cursor);              // action plays its own confirm
    }
  }

  // ---- time step ----
  bool adventuring = (view==VIEW_ADV && pet.stage>=1 && pet.stage<=3);
  float dtMin=(now-lastTickMs)/60000.0f;
  if(dtMin>0){ advance(dtMin,adventuring); lastTickMs=now; }

  // ---- combat: energy is stamina, no power dependency ----
  if(adventuring){
    retreating=(pet.energy<=RETREAT_ENERGY);
    if(retreating){
      pet.energy=clampf(pet.energy+ENERGY_REGEN*dtMin);       // catch breath
    } else {
      if(!encActive && now-lastEncounter>ENCOUNTER_MS){ startEncounter(); encApplied=false; }
      if(encActive){
        if(!encApplied && now-encStart>CLASH_MS){
          resolveEncounter(); pet.energy=clampf(pet.energy-FIGHT_ENERGY); encApplied=true;
          if(pet.health<=0){ pet.stage=4; pet.asleep=false; encActive=false; view=VIEW_HOME;
                             sfxDie(); saveState(); LOGF("DIED in battle at Lv%d age %dm\n",pet.level,(int)pet.ageMin); }
        }
        if(now-encStart>2200){ encActive=false; lastEncounter=now; saveState(); }
      }
    }
  } else { encActive=false; retreating=false; }

  // ---- blink / autosave ----
  if(now>nextBlink){ blinkUntil=now+140; nextBlink=now+2200+random(2600); }
  if(now-lastSave>SAVE_EVERY_MS) saveState();

  // ---- periodic status log (brownout / health watch during dev) ----
#if STICKPET_LOG
  static uint32_t lastLog=0;
  if(now-lastLog>10000){ lastLog=now;
    int b=M5.Power.getBatteryLevel();
    LOGF("st=%d lv=%d hu=%d ha=%d en=%d he=%d bat=%d chg=%d view=%d%s\n",
      pet.stage,pet.level,(int)pet.hunger,(int)pet.happy,(int)pet.energy,(int)pet.health,
      b,(int)M5.Power.isCharging(),view,muted?" [mute]":"");
  }
#endif

  // ---- render ----
  if(now-lastFrame>=FRAME_MS){
    if(pet.stage==4) renderDead(now);
    else if(view==VIEW_ADV) renderAdventure(now);
    else if(view==VIEW_RPT) renderReport(now);
    else renderHome(now);
    lastFrame=now;
  }
  delay(4);
}
