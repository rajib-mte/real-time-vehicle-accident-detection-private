// Host test for src/sms_format.cpp:
//   g++ -std=c++17 -Isrc tools/sms_format_test.cpp src/sms_format.cpp -o smstest && ./smstest
#include <cstdio>
#include <cmath>
#include <cstring>
#include "sms_format.h"
static void t(const char* name, SmsInfo i){ char b[200]; size_t n=formatAccidentSms(i,b,sizeof b); printf("[%s] (%zu chars)\n%s\n\n",name,n,b); }
int main(){
  t("normal: live GPS fix", {"accident",6.8f,1,34.5f,true,23.810312,90.412518,0,"Evercare Hospital Dhaka",2.0f});
  t("rollover, AI NO", {"rollover",3.2f,0,31.0f,true,23.75271,90.38163,0,"Square Hospital",0.4f});
  t("max-length edge", {"over_temp",15.97f,1,-10.5f,true,-33.868820,-151.209295,1,"Shaheed Suhrawardy Medical College Hospital Dha",12.3f});
  t("no GPS fix (preset location)", {"accident",4.4f,1,29.0f,true,23.8103,90.4125,2,"Dhaka Medical College Hospital",9.6f});
  t("no position at all, no temp, no model", {"accident",9.1f,-1,NAN,false,0,0,2,nullptr,0});
  // fuzz: never > 160
  int bad=0; char b[200];
  for(int k=0;k<20000;k++){ char nm[60]; int L=k%48; memset(nm,'H',L); nm[L]=0;
    SmsInfo i{ (k%3==0)?"accident":(k%3==1)?"rollover":"over_temp", (float)(k%160)/10.f, (k%3)-1, (k%7)?(float)(k%1500)/10.f-20:NAN, (k%5)!=0, -89.99999+(k%180), -179.99999+(k%360), k%3, (k%4)?nm:nullptr, (float)(k%999)/10.f};
    size_t n=formatAccidentSms(i,b,sizeof b); if(n>160||strncmp(b,"ACC",3)&&strncmp(b,"ALERT",5)||!strstr(b,"Peak")) bad++; }
  printf("fuzz 20000 cases: %d violations (len>160 or type/peak missing)\n",bad);
}
