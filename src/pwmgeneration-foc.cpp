/*
 * This file is part of the stm32-sine project.
 *
 * Copyright (C) 2015 Johannes Huebner <dev@johanneshuebner.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rcc.h>
#include "pwmgeneration.h"
#include "hwdefs.h"
#include "params.h"
#include "inc_encoder.h"
#include "sine_core.h"
#include "fu.h"
#include "errormessage.h"
#include "digio.h"
#include "anain.h"
#include "my_math.h"
#include "foc.h"
#include "picontroller.h"

#define FRQ_TO_ANGLE(frq) FP_TOINT((frq << SineCore::BITS) / pwmfrq)
#define DIGIT_TO_DEGREE(a) FP_FROMINT(angle) / (65536 / 360)

static int initwait = 0;
static int32_t qlimit = 0;
static const s32fp dcCurFac = FP_FROMFLT(0.81649658092772603273 * 1.05); //sqrt(2/3)*1.05 (inverter losses)
static tim_oc_id ocChannels[3];
static int curki = 0;
static int curkp = 0;
static PiController qController;
static PiController dController;
static PiController fwController;

void PwmGeneration::Run()
{
   if (opmode == MOD_MANUAL || opmode == MOD_RUN)
   {
      static s32fp idcFiltered = 0;
      int dir = Param::GetInt(Param::dir);
      float kpfrqgain = Param::GetFloat(Param::curkpfrqgain);
      int kifrqgain = Param::GetInt(Param::curkifrqgain);
      s32fp id, iq;

      Encoder::UpdateRotorAngle(dir);

      CalcNextAngleSync(dir);
      FOC::SetAngle(angle);
      static s32fp frqFiltered = 0;
      frqFiltered = IIRFILTER(frqFiltered, frq, 8);

      int moddedKp = curkp + kpfrqgain * FP_TOINT(frqFiltered);
      int moddedKi = curki + kifrqgain * FP_TOINT(frqFiltered);

      qController.SetIntegralGain(moddedKi);
      dController.SetIntegralGain(moddedKi);
      qController.SetProportionalGain(moddedKp);
      dController.SetProportionalGain(moddedKp);


      ProcessCurrents(id, iq);


      if (opmode == MOD_MANUAL)
      {
         dController.SetRef(Param::Get(Param::manualid));
         qController.SetRef(Param::Get(Param::manualiq));
      }

      int32_t ud = dController.Run(id);
      qlimit = FOC::GetQLimit(ud);
      qController.SetMinMaxY(dir < 0 ? -qlimit : -Param::GetInt(Param::negQLim) * qlimit, dir > 0 ? qlimit : Param::GetInt(Param::negQLim) * qlimit);
      int32_t uq = qController.Run(iq);

      FOC::InvParkClarke(ud, uq);

      s32fp idc = (iq * uq + id * ud) / FOC::GetMaximumModulationIndex();
      idc = FP_MUL(idc, dcCurFac);
      idcFiltered = IIRFILTER(idcFiltered, idc, Param::GetInt(Param::idcflt));

      Param::SetFixed(Param::fstat, frq);
      Param::SetFixed(Param::angle, DIGIT_TO_DEGREE(angle));
      Param::SetFixed(Param::idc, idcFiltered);
      Param::SetInt(Param::amp, qlimit);
      Param::SetInt(Param::uq, uq);
      Param::SetInt(Param::ud, ud);

      /* Shut down PWM on stopped motor or init phase */
      if ((0 == frq && 0 == dController.GetRef() && 0 == qController.GetRef()) || initwait > 0)
      {
         timer_disable_break_main_output(PWM_TIMER);
         dController.ResetIntegrator();
         qController.ResetIntegrator();
         fwController.ResetIntegrator();
         RunOffsetCalibration();
      }
      else
      {
         timer_enable_break_main_output(PWM_TIMER);
      }

      for (int i = 0; i < 3; i++)
      {
         timer_set_oc_value(PWM_TIMER, ocChannels[i], FOC::DutyCycles[i] >> shiftForTimer);
      }
   }
   else if (opmode == MOD_BOOST || opmode == MOD_BUCK)
   {
      initwait = 0;
      Charge();
   }
   else if (opmode == MOD_ACHEAT)
   {
      initwait = 0;
      AcHeat();
   }
}

void PwmGeneration::SetTorquePercent(float torquePercent)
{
   s32fp fwFrqStart = Param::Get(Param::fwFrqStart) ;
   s32fp fwFrqMid = Param::Get(Param::fwFrqMid);
   s32fp fwFrqEnd = Param::Get(Param::fwFrqEnd);
   float throtcur = Param::GetFloat(Param::throtcur);
   float idiqSplit = Param::GetFloat(Param::idiqsplit);
   float is = throtcur * torquePercent;
   float fwId = 0;
   float fwIdMid = Param::GetFloat(Param::fwIdMid);
   float fwIdEnd = Param::GetFloat(Param::fwIdEnd);
   int maxOverdrive = Param::GetInt(Param::overdrive);

   float fwIq = 0;
   float fwIqMid = Param::GetFloat(Param::fwIqMid);
   float fwIqEnd = Param::GetFloat(Param::fwIqEnd);

   static s32fp frqFiltered = 0;

   frqFiltered = IIRFILTER(frqFiltered, frq, Param::GetInt(Param::fwfrqflt));
   
   if(frqFiltered > fwFrqEnd){ 
      fwId = fwIdEnd;
      fwIq = fwIqEnd;
   }else if(frqFiltered > fwFrqMid){
      fwId = fwIdMid + (fwIdEnd-fwIdMid) * (frqFiltered - fwFrqMid)/(fwFrqEnd-fwFrqMid);
      fwIq = fwIqMid + (fwIqEnd-fwIqMid) * (frqFiltered - fwFrqMid)/(fwFrqEnd-fwFrqMid);
   }else if(frqFiltered > fwFrqStart){
      fwId = fwIdMid * (frqFiltered - fwFrqStart)/(fwFrqMid-fwFrqStart);
      fwIq = fwIqMid * (frqFiltered - fwFrqStart)/(fwFrqMid-fwFrqStart);
   }else{
      fwId = 0;
      fwIq = 0;
   }

   Param::SetFloat(Param::ifw, fwId);
   Param::SetFloat(Param::ifwq, fwIq);

   float id = idiqSplit * is / 100.0f;
   id = -ABS(id) - fwId; 
   id = MAX(id, -100 * throtcur); //Limit id to 100% throttle. MAX function because negative

   float iq = (100.0f-idiqSplit) * is / 100.0f;
   iq = ABS(iq + fwIq);
   iq = MIN(iq,100*throtcur);

   is += ABS(fwId) + ABS(fwIq); 
   is = MIN(is, maxOverdrive * throtcur);

   s32fp iAbs = fp_sqrt(FP_FROMFLT(iq * iq) + FP_FROMFLT(id * id));
   s32fp norm = FP_FROMFLT(1.0f);

   if (FP_TOFLOAT(iAbs) > is){
      norm = FP_DIV(FP_FROMFLT(is), iAbs );
   }

   Param::SetFloat(Param::iAbs, FP_TOFLOAT(iAbs));
   Param::SetFloat(Param::norm, FP_TOFLOAT(norm));
   Param::SetFloat(Param::is, is);
   Param::SetFloat(Param::idReq, (FP_MUL(FP_FROMFLT(id),norm)));
   Param::SetFloat(Param::iqReq, iq);

   s32fp iqRef = SIGN(torquePercent) * ABS(FP_MUL(FP_FROMFLT(ABS(iq)),norm));
   qController.SetRef(iqRef);
   dController.SetRef(FP_MUL(FP_FROMFLT(id),norm));
}

void PwmGeneration::SetControllerGains(int kp, int ki, int fwkp, int fwki)
{
   qController.SetGains(kp, ki);
   dController.SetGains(kp, ki);
   fwController.SetGains(fwkp, fwki);
   curki = ki;
   curkp = kp;
}

void PwmGeneration::PwmInit()
{
   int32_t maxVd = FOC::GetMaximumModulationIndex() - 2000;
   pwmfrq = TimerSetup(Param::GetInt(Param::deadtime), Param::GetInt(Param::pwmpol));
   slipIncr = FRQ_TO_ANGLE(fslip);
   Encoder::SetPwmFrequency(pwmfrq);
   initwait = pwmfrq / 2; //0.5s
   qController.ResetIntegrator();
   qController.SetCallingFrequency(pwmfrq);
   qController.SetMinMaxY(-maxVd, maxVd);
   dController.ResetIntegrator();
   dController.SetCallingFrequency(pwmfrq);
   dController.SetMinMaxY(-maxVd, maxVd);
   fwController.ResetIntegrator();
   fwController.SetCallingFrequency(100);
   fwController.SetMinMaxY(-100 * Param::Get(Param::throtcur), 0); //allow 100% of max current for extra field weakening

   if ((Param::GetInt(Param::pinswap) & SWAP_PWM13) > 0)
   {
      ocChannels[0] = TIM_OC3;
      ocChannels[1] = TIM_OC2;
      ocChannels[2] = TIM_OC1;
   }
   else if ((Param::GetInt(Param::pinswap) & SWAP_PWM23) > 0)
   {
      ocChannels[0] = TIM_OC1;
      ocChannels[1] = TIM_OC3;
      ocChannels[2] = TIM_OC2;
   }
   else
   {
      ocChannels[0] = TIM_OC1;
      ocChannels[1] = TIM_OC2;
      ocChannels[2] = TIM_OC3;
   }

   if (opmode == MOD_ACHEAT)
      AcHeatTimerSetup();
}

s32fp PwmGeneration::ProcessCurrents(s32fp& id, s32fp& iq)
{
   s32fp ocurlim = Param::Get(Param::ocurlim);
   ocurlim = ABS(ocurlim);

   if (initwait > 0)
   {
      initwait--;
   }

   s32fp il1 = GetCurrent(AnaIn::il1, ilofs[0], Param::Get(Param::il1gain));
   s32fp il2 = GetCurrent(AnaIn::il2, ilofs[1], Param::Get(Param::il2gain));

   if ((Param::GetInt(Param::pinswap) & SWAP_CURRENTS) > 0)
      FOC::ParkClarke(il2, il1);
   else
      FOC::ParkClarke(il1, il2);
   id = FOC::id;
   iq = FOC::iq;

   Param::SetFixed(Param::id, FOC::id);
   Param::SetFixed(Param::iq, FOC::iq);
   Param::SetFixed(Param::il1, il1);
   Param::SetFixed(Param::il2, il2);

   if (ABS(il1) > ocurlim || ABS(il2) > ocurlim)
   {
      Param::SetInt(Param::opmode, MOD_OFF);
      tripped = true;
      ErrorMessage::Post(ERR_OVERCURRENT_SW);
   }

   return 0;
}

void PwmGeneration::CalcNextAngleSync(int dir)
{
   static s32fp frqFiltered = 0;
   frqFiltered = IIRFILTER(frqFiltered, frq, 8);

   if (Encoder::SeenNorthSignal())
   {
      uint16_t syncOfs = Param::GetInt(Param::syncofs);
      uint16_t rotorAngle = Encoder::GetRotorAngle();
      int syncadv = Param::GetInt(Param::syncadv);
      int syncadvOff = Param::GetInt(Param::syncadvOffs);
      
      if(frq > Param::Get(Param::syncadvEnd)){
         syncadv -= syncadvOff;
         
      }else if(frq > Param::Get(Param::syncadvStart)){
         syncadv -= (syncadvOff * ((frq) - Param::Get(Param::syncadvStart))/(Param::Get(Param::syncadvEnd)-Param::Get(Param::syncadvStart)));
      }
      Param::SetInt(Param::syncAdvFinal, syncadv);
      syncadv = frqFiltered * syncadv;
      syncadv = MAX(0, syncadv);
      
      //Compensate rotor movement that happened between sampling and processing
      syncOfs += FP_TOINT(dir * syncadv);
      Param::SetInt(Param::syncOffFinal, syncOfs);
      angle = polePairRatio * rotorAngle + syncOfs;
      frq = polePairRatio * Encoder::GetRotorFrequency();
   }
   else
   {
      frq = fslip;
      angle += dir * FRQ_TO_ANGLE(fslip);
   }
}

void PwmGeneration::RunOffsetCalibration()
{
   static int il1Avg = 0, il2Avg = 0, samples = 0;
   const int offsetSamples = 512;

   if (samples < offsetSamples)
   {
      il1Avg += AnaIn::il1.Get();
      il2Avg += AnaIn::il2.Get();
      samples++;
   }
   else
   {
      SetCurrentOffset(il1Avg / offsetSamples, il2Avg / offsetSamples);
      il1Avg = il2Avg = 0;
      samples = 0;
   }
}
