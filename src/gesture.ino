// ========== Gesture Light v2: Follow → Lock (1.2s) → 3s Cooldown → Move-to-Unlock → Palm-Cover OFF → Wake ==========
// Serial: 115200 baud
// FIXED LOCKING VERSION: Simplified stability detection

#include <math.h>

// ---------------- TUNABLES ----------------
const int   PIN_TRIG = 8, PIN_ECHO = 7;
const int   PIN_LED1 = 5, PIN_LED2 = 6, PIN_LED3 = 9;
const int   PIN_BUZZ = 11;

// Distance window & filtering
const float DIST_MIN_CM = 2.0f;
const float DIST_MAX_CM = 20.0f;
const unsigned long SAMPLE_MS = 40;
const float EMA_ALPHA = 0.18f;
const float OUT_EASE  = 0.22f;
const float MAX_STEP_CM = 4.0f;
const float HAND_PRESENT_MARGIN = 0.2f;

// Locking - SIMPLIFIED
const unsigned long STABLE_MS      = 800; // steady time to lock
const float         STABLE_BAND_CM = 4.0f; // INCREASED: more forgiving band
const unsigned long COOLDOWN_MS    = 3000;

// Extra guard: block locking right after wake/unlock
const unsigned long LOCK_ARM_MS_AFTER_WAKE = 1500;

// Move-to-unlock
const float         UNLOCK_MOVE_CM = 3.0f;
const unsigned long UNLOCK_HOLD_MS = 250;

// Palm-cover OFF & Wake
const float         OFF_NEAR_CM      = 2.8f;
const unsigned long OFF_HOLD_MS      = 1200;
const unsigned long OFF_IGNORE_MS    = 2500;
const unsigned long WAKE_PRESENT_MS  = 400;
const float         FAR_MARGIN_CM    = 0.3f;
const unsigned long WAKE_ARM_MS      = 300;

// Brightness behavior
const float BRIGHT_FREEZE_MARGIN_CM  = 0.4f;
const float FULL_BRIGHT_CM           = 3.3f;
const float BRIGHT_ZONE_CM           = 3.6f;
const unsigned long NEAR_LOCK_MS     = 700;

// ---------------- STATE ----------------
enum Mode { FOLLOW, LOCKED, OFF };
Mode mode = FOLLOW;

const int LEDS[3] = {PIN_LED1, PIN_LED2, PIN_LED3};

float emaDist = DIST_MAX_CM, lastForCap = DIST_MAX_CM;

// *** Single-channel brightness (all LEDs equal) ***
int   targetLevel = 0;
float outLevel    = 0.0f;

int   lockLevel   = -1;
float lockRefDist = DIST_MAX_CM;

unsigned long lastSamp=0, lockAt=0, timeoutCount=0;
bool  inCooldown=false;

// SIMPLIFIED history for stability
struct Smp { unsigned long t; float d; };
Smp hist[32]; // Reduced size
int hHead=0;

// unlock tracking
unsigned long unlockStart=0;

// OFF / Wake timers
unsigned long nearStart = 0;
unsigned long wakeStart = 0;
unsigned long offAt     = 0;

// ---- WAKE ARM state ----
bool          wakeArmed   = false;
unsigned long farStart    = 0;

// ---- Lock-arm after wake/unlock ----
unsigned long lockArmBlockedUntil = 0;

// ---- Stability tracking ----
unsigned long stableStart = 0;
float stableStartDist = 0;

// ---- Near-lock tracking ----
unsigned long brightStart = 0;  // ADDED: Declare brightStart here

// ---------- SERIAL LOGGING ----------
void logLabeled(unsigned long now, float dist, Mode mode, int bright, const char* extra = "") {
  static unsigned long lastLog = 0;
  if (now - lastLog < 200) return; // Log less frequently
  lastLog = now;
  
  Serial.print("T:"); Serial.print(now); 
  Serial.print(" D:"); Serial.print(dist,1); 
  Serial.print(" S:"); Serial.print(mode==FOLLOW?"FOL":(mode==LOCKED?"LCK":"OFF"));
  Serial.print(" B:"); Serial.print(bright);
  Serial.print(" "); Serial.println(extra);
}

// ---------------- BUZZER ----------------
struct Beep { unsigned long onMs, gapMs; int freq; };
Beep bq[4]; int bHead=0, bTail=0; bool buzzing=false,inGap=false; unsigned long phaseEnd=0;

void queueBeep(int f,unsigned long onMs,unsigned long gapMs){
  int n=(bTail+1)%4; if(n==bHead) return; bq[bTail]={onMs,gapMs,f}; bTail=n;
}
void beepLock(){ queueBeep(2000,120,0); }
void beepWake(){ queueBeep(1700,90,0); }
void beepOff() { queueBeep(800, 300,0); }

void runBuzzer(){
  unsigned long now=millis();
  if(!buzzing && bHead!=bTail){
    tone(PIN_BUZZ,bq[bHead].freq);
    buzzing=true; inGap=false; phaseEnd=now+bq[bHead].onMs;
  }else if(buzzing && !inGap && now>=phaseEnd){
    noTone(PIN_BUZZ);
    inGap=true; phaseEnd=now+bq[bHead].gapMs;
    if(bq[bHead].gapMs==0){ bHead=(bHead+1)%4; buzzing=false; }
  }else if(buzzing && inGap && now>=phaseEnd){
    bHead=(bHead+1)%4; buzzing=false;
  }
}

// ---------------- SONAR ----------------
long readPulseCm(){
  digitalWrite(PIN_TRIG,LOW); delayMicroseconds(2);
  digitalWrite(PIN_TRIG,HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG,LOW);
  unsigned long dur=pulseIn(PIN_ECHO,HIGH,30000UL);
  if(!dur){ timeoutCount++; return -1; }
  return (long)(dur/58);
}

static inline float median3(float a,float b,float c){ 
  if(a>b){float t=a;a=b;b=t;} 
  if(b>c){float t=b;b=c;c=t;} 
  if(a>b){float t=a;a=b;b=t;} 
  return b; 
}

float readDistanceFiltered(){
  long a=readPulseCm(); delayMicroseconds(1000);
  long b=readPulseCm(); delayMicroseconds(1000);
  long c=readPulseCm();
  float m=-1;
  if(a>0&&b>0&&c>0) m=median3(a,b,c);
  else if(a>0&&b>0) m=(a+b)/2.0f;
  else if(b>0&&c>0) m=(b+c)/2.0f;
  else if(a>0||b>0||c>0) m=(a>0?a:(b>0?b:c));
  else {
    timeoutCount++;
    return emaDist;
  }
  
  if(m>0){
    if(m<DIST_MIN_CM) m=DIST_MIN_CM;
    if(m>DIST_MAX_CM) m=DIST_MAX_CM;
    float newEma = EMA_ALPHA*m + (1.0f-EMA_ALPHA)*emaDist;
    float d=newEma-lastForCap;
    if(d> MAX_STEP_CM) newEma=lastForCap+MAX_STEP_CM;
    if(d<-MAX_STEP_CM) newEma=lastForCap-MAX_STEP_CM;
    emaDist=newEma; lastForCap=newEma;
  }
  return emaDist;
}

// ---------------- SIMPLIFIED STABILITY DETECTION ----------------
void pushHist(float d){ 
  hist[hHead]={millis(),d}; 
  hHead=(hHead+1)%32;
}

bool isDistanceStable() {
  if (hHead < 2) return false;
  
  float currentDist = emaDist;
  float deviation = fabsf(currentDist - stableStartDist);
  
  // Check recent history for noise
  float minDist = currentDist, maxDist = currentDist;
  for(int i = 0; i < 8; i++) { // INCREASE from 5 to 8 samples for smoother checking
    int idx = (hHead - 1 - i + 32) % 32;
    float d = hist[idx].d;
    if(d < minDist) minDist = d;
    if(d > maxDist) maxDist = d;
  }
  
  float noiseBand = maxDist - minDist;
  
  // Make band checking more forgiving:
  return (deviation <= (STABLE_BAND_CM / 2.0f)) && (noiseBand <= STABLE_BAND_CM * 1.2f); // ADDED 1.2x multiplier
}


bool handPresent(){
  return (emaDist < (DIST_MAX_CM - HAND_PRESENT_MARGIN)) &&
         (emaDist > (DIST_MIN_CM + HAND_PRESENT_MARGIN));
}

bool farRegion(){ 
  return emaDist >= (DIST_MAX_CM - FAR_MARGIN_CM); 
}

// ---------------- BRIGHTNESS ----------------
int mapDistToLevel(float cm){
  if(cm <= (OFF_NEAR_CM + BRIGHT_FREEZE_MARGIN_CM)) return targetLevel;
  if(cm <= FULL_BRIGHT_CM) return 255;
  if(cm > DIST_MAX_CM) cm = DIST_MAX_CM;
  
  float t = (cm - FULL_BRIGHT_CM) / (DIST_MAX_CM - FULL_BRIGHT_CM);
  int L = (int)((1.0f - t) * 255.0f);
  if(L < 0) L = 0; 
  if(L > 255) L = 255;
  return L;
}

void writeUniform(float level){
  if(level < 0) level = 0; 
  if(level > 255) level = 255;
  int v = (int)level;
  analogWrite(LEDS[0], v);
  analogWrite(LEDS[1], v);
  analogWrite(LEDS[2], v);
}

// ---------------- SETUP ----------------
void setup(){
  Serial.begin(115200);
  Serial.println("Gesture Light - FIXED LOCKING");

  pinMode(PIN_TRIG, OUTPUT); 
  pinMode(PIN_ECHO, INPUT);
  for(int i=0; i<3; i++){ 
    pinMode(LEDS[i], OUTPUT); 
    analogWrite(LEDS[i], 0); 
  }
  pinMode(PIN_BUZZ, OUTPUT);

  // Initialize all state variables
  emaDist = DIST_MAX_CM; 
  lastForCap = emaDist; 
  lastSamp = millis();
  mode = FOLLOW; 
  inCooldown = false; 
  lockLevel = -1; 
  lockAt = 0; 
  lockRefDist = DIST_MAX_CM;
  unlockStart = 0; 
  nearStart = 0; 
  wakeStart = 0; 
  offAt = 0;
  wakeArmed = false; 
  farStart = 0;
  lockArmBlockedUntil = 0;
  stableStart = 0;
  stableStartDist = DIST_MAX_CM;
  brightStart = 0;  // ADDED: Initialize brightStart

  // Initialize history
  for(int i=0; i<32; i++) {
    hist[i] = {0, DIST_MAX_CM};
  }

  queueBeep(1800, 120, 0);
}

// ---------------- MAIN LOOP - SIMPLIFIED LOCKING LOGIC ----------------
void loop(){
  unsigned long now = millis();

  // HARD IGNORE WINDOW after OFF
  if(mode == OFF && (now - offAt) < OFF_IGNORE_MS){
    targetLevel = 0;
    outLevel += OUT_EASE * (0 - outLevel);
    writeUniform(outLevel);
    runBuzzer();
    return;
  }

  if(now - lastSamp >= SAMPLE_MS){
    lastSamp += SAMPLE_MS;

    float currentDist = readDistanceFiltered();
    pushHist(currentDist);

    bool present = handPresent();
    bool isFar = farRegion();
    
    char debugInfo[60] = "";

    // ===== OFF gesture detection =====
    if(mode != OFF && !inCooldown && currentDist <= OFF_NEAR_CM){
      if(nearStart == 0) nearStart = now;
      if(now - nearStart >= OFF_HOLD_MS){
        mode = OFF;
        offAt = now;
        targetLevel = 0;
        beepOff();
        wakeArmed = false;
        farStart = 0;
        stableStart = 0;
        brightStart = 0;  // ADDED: Reset brightStart
        strcpy(debugInfo, "OFF");
      }
    } else {
      nearStart = 0;
    }

    if(mode == FOLLOW){
      if(inCooldown){
        targetLevel = lockLevel;
        if(now - lockAt >= COOLDOWN_MS) inCooldown = false;
      } else {
        // Update brightness based on distance
        if(present){
          targetLevel = mapDistToLevel(currentDist);
          if(lockLevel < 0) lockLevel = targetLevel;
        } else {
          targetLevel = (lockLevel >= 0) ? lockLevel : 0;
        }

        // SIMPLIFIED LOCKING LOGIC
        bool lockArmReady = (now >= lockArmBlockedUntil);
        
        if(present && lockArmReady){
          if(stableStart == 0){
            // Start stability timer
            stableStart = now;
            stableStartDist = currentDist;
            sprintf(debugInfo, "STABLE_START D:%.1f", stableStartDist);
          } else {
            // Check if we're still stable
            if(isDistanceStable()){
              unsigned long stableTime = now - stableStart;
              if(stableTime >= STABLE_MS){
                // LOCK!
                mode = LOCKED;
                lockLevel = targetLevel;
                lockRefDist = currentDist;
                lockAt = now;
                inCooldown = true;
                beepLock();
                stableStart = 0;
                brightStart = 0;  // ADDED: Reset brightStart
                sprintf(debugInfo, "LOCKED T:%lu D:%.1f", stableTime, lockRefDist);
              } else {
                sprintf(debugInfo, "STABLE_%lu/%lu D:%.1f", stableTime, STABLE_MS, currentDist);
              }
            } else {
              // Not stable - reset timer
              stableStart = 0;
              strcpy(debugInfo, "UNSTABLE");
            }
          }
        } else {
          // No hand present or lock not armed - reset stability
          if(stableStart != 0){
            strcpy(debugInfo, "STABLE_RESET");
          }
          stableStart = 0;
        }
        
        // Near-lock shortcut (works at close range)
        bool inBrightZone = (currentDist <= BRIGHT_ZONE_CM) && present;
        if(inBrightZone && lockArmReady) {
          if(brightStart == 0) brightStart = now;
          if(now - brightStart >= NEAR_LOCK_MS){
            mode = LOCKED;
            lockLevel = 255;
            lockRefDist = currentDist;
            lockAt = now;
            inCooldown = true;
            beepLock();
            brightStart = 0;
            stableStart = 0;
            strcpy(debugInfo, "NEAR_LOCK");
          }
        } else {
          brightStart = 0;
        }
      }

    } else if(mode == LOCKED){
      targetLevel = lockLevel;

      if(inCooldown){
        if(now - lockAt >= COOLDOWN_MS){ 
          inCooldown = false; 
        }
      } else if(present){
        // Unlock detection
        float dev = fabsf(currentDist - lockRefDist);
        if(dev >= UNLOCK_MOVE_CM){
          if(unlockStart == 0) unlockStart = now;
          if(now - unlockStart >= UNLOCK_HOLD_MS){
            mode = FOLLOW;
            unlockStart = 0;
            lockArmBlockedUntil = now + LOCK_ARM_MS_AFTER_WAKE;
            stableStart = 0;
            brightStart = 0;  // ADDED: Reset brightStart
            strcpy(debugInfo, "UNLOCKED");
          } else {
            sprintf(debugInfo, "UNLOCKING_%lu", UNLOCK_HOLD_MS - (now - unlockStart));
          }
        } else {
          unlockStart = 0;
        }
      } else {
        unlockStart = 0;
      }

    } else { // OFF mode
      targetLevel = 0;

      // Wake logic
      if(!wakeArmed){
        if(isFar){
          if(farStart == 0) farStart = now;
          if(now - farStart >= WAKE_ARM_MS){
            wakeArmed = true;
          }
        } else {
          farStart = 0;
        }
      }

      if(wakeArmed && present){
        if(wakeStart == 0) wakeStart = now;
        if(now - wakeStart >= WAKE_PRESENT_MS){
          mode = FOLLOW;
          beepWake();
          lockArmBlockedUntil = now + LOCK_ARM_MS_AFTER_WAKE;
          wakeArmed = false;
          farStart = 0;
          wakeStart = 0;
          stableStart = 0;
          brightStart = 0;  // ADDED: Reset brightStart
          strcpy(debugInfo, "WOKE");
        }
      } else {
        wakeStart = 0;
      }
    }

    // Smooth output
    outLevel += OUT_EASE * (targetLevel - outLevel);
    writeUniform(outLevel);

    // Log if no specific debug info
    if(debugInfo[0] == '\0') {
      if(mode == FOLLOW) {
        if(present) {
          if(stableStart > 0) {
            unsigned long stableTime = now - stableStart;
            sprintf(debugInfo, "FOL_STABLE_%lu/%lu", stableTime, STABLE_MS);
          } else {
            strcpy(debugInfo, "FOL_PRESENT");
          }
        } else {
          strcpy(debugInfo, "FOL_NO_HAND");
        }
      } else if(mode == LOCKED) {
        strcpy(debugInfo, "LCK_ACTIVE");
      } else {
        strcpy(debugInfo, "OFF_ACTIVE");
      }
    }
    
    logLabeled(now, currentDist, mode, (int)outLevel, debugInfo);
  }

  runBuzzer();
}