/*  Copyright 2003-2006 Guillaume Duhamel
    Copyright 2005-2006 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file scu.c
    \brief SCU emulation functions.
*/

#include <stdlib.h>
#include "scu.h"
#include "debug.h"
#include "memory.h"
#include "sh2core.h"
#include "yabause.h"
#include <inttypes.h>

#ifdef OPTIMIZED_DMA
# include "cs2.h"
# include "scsp.h"
# include "vdp1.h"
# include "vdp2.h"
#endif


Scu scu_regs;
Scu * ScuRegs;
scudspregs_struct * ScuDsp;
scubp_struct * ScuBP;
static int incFlg[4] = { 0 };
static void ScuTestInterruptMask(void);

void ScuRemoveInterruptByCPU(u32 pre, u32 after);
void step_dsp_dma(scudspregs_struct *sc);
void ScuSendDMAEnd(u32 mode);

//#define LOG
#define OLD_DMA 0
//////////////////////////////////////////////////////////////////////////////

int ScuInit(void) {
   int i;

	ScuRegs = &scu_regs;
	memset(ScuRegs, 0, sizeof(Scu));
	memset(&ScuRegs->dma0, 0, sizeof(ScuRegs->dma0));
	memset(&ScuRegs->dma1, 0, sizeof(ScuRegs->dma1));
	memset(&ScuRegs->dma2, 0, sizeof(ScuRegs->dma2));

   if ((ScuDsp = (scudspregs_struct *) calloc(1, sizeof(scudspregs_struct))) == NULL)
      return -1;

   if ((ScuBP = (scubp_struct *) calloc(1, sizeof(scubp_struct))) == NULL)
      return -1;

   for (i = 0; i < MAX_BREAKPOINTS; i++)
      ScuBP->codebreakpoint[i].addr = 0xFFFFFFFF;
   ScuBP->numcodebreakpoints = 0;
   ScuBP->BreakpointCallBack=NULL;
   ScuBP->inbreakpoint=0;

   for( int j=0; i<4; j++ ){
      for( int i=0; i<64; i++ ){
         ScuDsp->MD[j][i] = -1;
      }
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void ScuDeInit(void) {
   if (ScuDsp)
      free(ScuDsp);
   ScuDsp = NULL;

   if (ScuBP)
      free(ScuBP);
   ScuBP = NULL;
}

//////////////////////////////////////////////////////////////////////////////

void ScuReset(void) {
   ScuRegs->D0AD = ScuRegs->D1AD = ScuRegs->D2AD = 0x101;
   ScuRegs->D0EN = ScuRegs->D1EN = ScuRegs->D2EN = 0x0;
   ScuRegs->D0MD = ScuRegs->D1MD = ScuRegs->D2MD = 0x7;
   ScuRegs->DSTP = 0x0;
   ScuRegs->DSTA = 0x0;

   ScuDsp->ProgControlPort.all = 0;
   ScuRegs->PDA = 0x0;

   ScuRegs->T1MD = 0x0;

   ScuRegs->IMS = 0xBFFF;
   //ScuRegs->IST = 0x0;

   ScuRegs->AIACK = 0x0;
   ScuRegs->ASR0 = ScuRegs->ASR1 = 0x0;
   ScuRegs->AREF = 0x0;

   ScuRegs->RSEL = 0x0;
   ScuRegs->VER = 0x04; // Looks like all consumer saturn's used at least version 4

   ScuRegs->timer0 = 0;
   ScuRegs->timer1 = 0;

   memset((void *)ScuRegs->interrupts, 0, sizeof(scuinterrupt_struct) * 30);
   ScuRegs->NumberOfInterrupts = 0;

   memset(&ScuRegs->dma0, 0, sizeof(ScuRegs->dma0));
   memset(&ScuRegs->dma1, 0, sizeof(ScuRegs->dma1));
   memset(&ScuRegs->dma2, 0, sizeof(ScuRegs->dma2));

}

//////////////////////////////////////////////////////////////////////////////

#ifdef OPTIMIZED_DMA

// Table of memory types for DMA optimization, in 512k (1<<19 byte) units:
//    0x00 = no special handling
//    0x12 = VDP1/2 RAM (8-bit organized, 16-bit copy unit)
//    0x22 = M68K RAM (16-bit organized, 16-bit copy unit)
//    0x23 = VDP2 color RAM (16-bit organized, 16-bit copy unit)
//    0x24 = SH-2 RAM (16-bit organized, 32-bit copy unit)
static const u8 DMAMemoryType[0x20000000>>19] = {
   [0x00200000>>19] = 0x24,
   [0x00280000>>19] = 0x24,
   [0x05A00000>>19] = 0x22,
   [0x05A80000>>19] = 0x22,
   [0x05C00000>>19] = 0x12,
   [0x05C00000>>19] = 0x12,
   [0x05E00000>>19] = 0x12,
   [0x05E80000>>19] = 0x12,
   [0x05F00000>>19] = 0x23,
   [0x06000000>>19] = 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                      0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
};

// Function to return the native pointer for an optimized address
#ifdef __GNUC__
__attribute__((always_inline))  // Force it inline for better performance
#endif
static INLINE void *DMAMemoryPointer(u32 address) {
   u32 page = (address & 0x1FF80000) >> 19;
   switch (DMAMemoryType[page]) {
      case 0x12:
         switch (page) {
            case 0x05C00000>>19: return &Vdp1Ram[address & 0x7FFFF];
            case 0x05E00000>>19: // fall through
            case 0x05E80000>>19: return &Vdp2Ram[address & 0x7FFFF];
            default: return NULL;
         }
      case 0x22:
         return &SoundRam[address & 0x7FFFF];
      case 0x23:
         return &Vdp2ColorRam[address & 0xFFF];
      case 0x24:
         if (page == 0x00200000>>19) {
            return &wram[address & 0xFFFFF];
         } else {
            return &wram[(address & 0xFFFFF) | 0x100000];
         }
      default:
         return NULL;
   }
}

#endif  // OPTIMIZED_DMA

//////////////////////////////////////////////////////////////////////////////

static u32 readgensrc(u8 num)
{
   if( num <= 7  ){
     incFlg[(num & 0x3)] |= ((num >> 2) & 0x01);
     // Finish Previous DMA operation
     if (ScuDsp->dsp_dma_wait > 0) {
       ScuDsp->dsp_dma_wait = 0;
       step_dsp_dma(ScuDsp);
     }

     return ScuDsp->MD[(num & 0x3)][ScuDsp->CT[(num & 0x3)]&0x3F];
   }else{
     if (num == 0x9)  // ALL
       return (u32)ScuDsp->ALU.part.L;
     else if (num == 0xA) // ALH
       return (u32)(ScuDsp->ALU.all >> 16); ////(u32)((ScuDsp->ALU.all & (u64)(0x0000ffffffff0000))  >> 16);
   }
#if 0
   switch(num) {
      case 0x0: // M0
         return ScuDsp->MD[0][ScuDsp->CT[0]];
      case 0x1: // M1
         return ScuDsp->MD[1][ScuDsp->CT[1]];
      case 0x2: // M2
         return ScuDsp->MD[2][ScuDsp->CT[2]];
      case 0x3: // M3
         return ScuDsp->MD[3][ScuDsp->CT[3]];
      case 0x4: // MC0
         val = ScuDsp->MD[0][ScuDsp->CT[0]];
         incFlg[0] = 1;
         return val;
      case 0x5: // MC1
         val = ScuDsp->MD[1][ScuDsp->CT[1]];
         incFlg[1] = 1;
         return val;
      case 0x6: // MC2
         val = ScuDsp->MD[2][ScuDsp->CT[2]];
         incFlg[2] = 1;
         return val;
      case 0x7: // MC3
         val = ScuDsp->MD[3][ScuDsp->CT[3]];
         incFlg[3] = 1;
         return val;
      case 0x9: // ALL
         return (u32)ScuDsp->ALU.part.L;
      case 0xA: // ALH
         return (u32)((ScuDsp->ALU.all & (u64)(0x0000ffffffff0000))  >> 16);
      default: break;
   }
#endif
   return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void writed1busdest(u8 num, u32 val)
{


  //LOG("writed1busdest [%d][%d] = %08X",num, ScuDsp->CT[0] & 0x3F, val);

  // Finish Previous DMA operation
  if (ScuDsp->dsp_dma_wait > 0) {
    ScuDsp->dsp_dma_wait = 0;
    step_dsp_dma(ScuDsp);
  }

   switch(num) {
      case 0x0:
          ScuDsp->MD[0][ScuDsp->CT[0]&0x3F] = val;
          incFlg[0] = 1;
          return;
      case 0x1:
         ScuDsp->MD[1][ScuDsp->CT[1] & 0x3F] = val;
         incFlg[1] = 1;
         return;
      case 0x2:
         ScuDsp->MD[2][ScuDsp->CT[2] & 0x3F] = val;
         incFlg[2] = 1;
         return;
      case 0x3:
         ScuDsp->MD[3][ScuDsp->CT[3] & 0x3F] = val;
         incFlg[3] = 1;
         return;
      case 0x4:
          ScuDsp->RX = val;
          return;
      case 0x5:
          ScuDsp->P.all = (signed)val;
          return;
      case 0x6:
          ScuDsp->RA0 = val;
          return;
      case 0x7:
          ScuDsp->WA0 = val;
          return;
      case 0xA:
          ScuDsp->LOP = (u16)val;
          return;
      case 0xB:
          ScuDsp->TOP = (u8)val;
          return;
      case 0xC:
          ScuDsp->CT[0] = (u8)val;
          incFlg[0] = 0;
          return;
      case 0xD:
          ScuDsp->CT[1] = (u8)val;
          incFlg[1] = 0;
          return;
      case 0xE:
          ScuDsp->CT[2] = (u8)val;
          incFlg[2] = 0;
          return;
      case 0xF:
          ScuDsp->CT[3] = (u8)val;
          incFlg[3] = 0;
          return;
      default: break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void writeloadimdest(u8 num, u32 val)
{

   //LOG("writeloadimdest [%d][%d] = %08X",num, ScuDsp->CT[0] & 0x3F, val);

  // Finish Previous DMA operation
  if (ScuDsp->dsp_dma_wait > 0) {
    ScuDsp->dsp_dma_wait = 0;
    step_dsp_dma(ScuDsp);
  }

   switch(num) {
      case 0x0: // MC0
         ScuDsp->MD[0][ScuDsp->CT[0] & 0x3F] = val;
         incFlg[0] = 1;
         return;
      case 0x1: // MC1
        ScuDsp->MD[1][ScuDsp->CT[1] & 0x3F] = val;
        incFlg[1] = 1;
        return;
      case 0x2: // MC2
        ScuDsp->MD[2][ScuDsp->CT[2] & 0x3F] = val;
          incFlg[2] = 1;
          return;
      case 0x3: // MC3
        ScuDsp->MD[3][ScuDsp->CT[3] & 0x3F] = val;
          incFlg[3] = 1;
          return;
      case 0x4: // RX
          ScuDsp->RX = val;
          return;
      case 0x5: // PL
          ScuDsp->P.all = (s32)val;
          return;
      case 0x6: // RA0
          val = (val & 0x1FFFFFF);
          ScuDsp->RA0 = val;
          return;
      case 0x7: // WA0
          val = (val & 0x1FFFFFF);
          ScuDsp->WA0 = val;
          return;
      case 0xA: // LOP
          ScuDsp->LOP = (u16)(val & 0x0FFF);
          return;
      case 0xC: // PC->TOP, PC
          ScuDsp->TOP = ScuDsp->PC+1;
          ScuDsp->jmpaddr = val;
          ScuDsp->delayed = 0;
          return;
      default:
        LOG("writeloadimdest BAD NUM %d,%d",num,val);
        break;
   }
}

//Half-Done
void dsp_dma01(scudspregs_struct *sc, u32 inst)
{
	u32 imm = ((inst & 0xFF));
	u8  sel = ((inst >> 8) & 0x03);
	//u8  addr = sc->CT[sel];
	u32 i;

	const u32 mode = (inst >> 15) & 0x7;
	const u32 add = (1 << (mode & 0x2)) &~1;

	for (i = 0; i < imm ; i++) {
		sc->MD[sel][sc->CT[sel] & 0x3F] = mem_read32_arr[MEM_GET_FUNC_ADDR(sc->RA0M << 2)]((sc->RA0M << 2));
		sc->CT[sel] = (sc->CT[sel] + 1) & 0x3F;
		sc->RA0M += (add >> 2);
	}

    sc->ProgControlPort.part.T0 = 0;
    sc->RA0 = sc->RA0M;
}


void dsp_dma_write_d0bus(scudspregs_struct *sc, int sel, int add, int count)
{
	int i;
	u32 Adr = (sc->WA0M << 2) & 0x0FFFFFFF;

	// A-BUS?
	if (Adr >= 0x02000000 && Adr < 0x05A00000) {
		if (add > 1) add = 1;
		WriteFunc32 write32_func = mem_write32_arr[MEM_GET_FUNC_ADDR(Adr)];
		for (i = 0; i < count; i++) {
			u32 Val = sc->MD[sel][sc->CT[sel] & 0x3F];
			Adr = (sc->WA0M << 2);
			write32_func(Adr, Val);
			sc->CT[sel] = (sc->CT[sel] + 1) & 0x3F;
			sc->WA0M += add;
		}
	} // B-BUS?
	else if (Adr >= 0x05A00000 && Adr < 0x06000000){
		if (add == 0) add = 1;
		WriteFunc16 write16_func = mem_write16_arr[MEM_GET_FUNC_ADDR(Adr)];
		for (i = 0; i < count; i++) {
			u32 Val = sc->MD[sel][sc->CT[sel] & 0x3F];
			write16_func(Adr, (Val>>16));
			write16_func(Adr+2, Val);
			sc->CT[sel] = (sc->CT[sel] + 1) & 0x3F;
			Adr += (add << 2);
		}
		sc->WA0M = sc->WA0M + ((add*count));
	} // CPU-BUS
	else {
		add >>= 1;
		if (add == 0) add = 1;
		for (i = 0; i < count; i++) {
			u32 Val = sc->MD[sel][sc->CT[sel] & 0x3F];
			Adr = (sc->WA0M << 2);
			T2WriteLong(wram, (Adr & 0xFFFFC) | 0x100000, Val);
			sc->CT[sel] = (sc->CT[sel] + 1) & 0x3F;
			sc->WA0M += add;
		}
	}

	sc->WA0 = sc->WA0M;
	sc->ProgControlPort.part.T0 = 0;
}

//DONE
void dsp_dma02(scudspregs_struct *sc, u32 inst)
{
	u32 imm = ((inst & 0xFF));
	u8 sel = ((inst >> 8) & 0x03);
	//u8 addr = sc->CT[sel];
	u8 add = 0x40 >> (7 - ((inst >> 15) & 0x07));

	dsp_dma_write_d0bus(sc, sel, add, imm);
}

//DONE
void dsp_dma03(scudspregs_struct *sc, u32 inst)
{
	u32 Counter = sc->dsp_dma_size;
	u32 i;
	int sel;

	sel = (inst >> 8) & 0x7;
	int index = 0;

	const u32 mode = (inst >> 15) & 0x7;
	const u32 add = (1 << (mode & 0x2)) &~1;

	u32 abus_check = ((sc->RA0M << 2) & 0x0FF00000);

	for (i = 0; i < Counter; i++) {
		if (sel == 0x04) {
			sc->ProgramRam[index] = mem_read32_arr[MEM_GET_FUNC_ADDR(sc->RA0M << 2)](sc->RA0M << 2);
			index++;
		}
		else {
			sc->MD[sel][sc->CT[sel] & 0x3F] = mem_read32_arr[MEM_GET_FUNC_ADDR(sc->RA0M << 2)](sc->RA0M << 2);
			sc->CT[sel]++;
			sc->CT[sel] &= 0x3F;
		}
		sc->RA0M += (add >> 2);
	}

	if (!(abus_check >= 0x02000000 && abus_check < 0x05900000)){
		sc->RA0 = sc->RA0M;
	}

    sc->ProgControlPort.part.T0 = 0;
}

//DONE
void dsp_dma04(scudspregs_struct *sc, u32 inst)
{
	u32 add = 0x40 >> (7 - ((inst >> 15) & 0x7));
	u32 sel = (inst >> 8) & 0x3;

	dsp_dma_write_d0bus(sc, sel, add, sc->dsp_dma_size);
}


void step_dsp_dma(scudspregs_struct *sc) {

  if (sc->ProgControlPort.part.T0 == 0) return;

  sc->dsp_dma_wait--;
  if (sc->dsp_dma_wait > 0) return;

  if (((sc->dsp_dma_instruction >> 10) & 0x1F) == 0x00)
  {
    dsp_dma01(ScuDsp, sc->dsp_dma_instruction);
  }
  else if (((sc->dsp_dma_instruction >> 10) & 0x1F) == 0x04)
  {
    dsp_dma02(ScuDsp, sc->dsp_dma_instruction);
  }
  else if (((sc->dsp_dma_instruction >> 11) & 0x0F) == 0x04)
  {
    dsp_dma03(ScuDsp, sc->dsp_dma_instruction);
  }
  else if (((sc->dsp_dma_instruction >> 10) & 0x1F) == 0x0C)
  {
    dsp_dma04(ScuDsp, sc->dsp_dma_instruction);
  }
  else if (((sc->dsp_dma_instruction >> 11) & 0x0F) == 0x08)
  {
    u32 saveRa0 = sc->RA0M;
    dsp_dma01(ScuDsp, sc->dsp_dma_instruction);
    sc->RA0 = saveRa0;
  }
  else if (((sc->dsp_dma_instruction >> 10) & 0x1F) == 0x14)
  {
    u32 saveWa0 = sc->WA0M;
    dsp_dma02(ScuDsp, sc->dsp_dma_instruction);
    sc->WA0 = saveWa0;
  }
  else if (((sc->dsp_dma_instruction >> 11) & 0x0F) == 0x0C)
  {
    u32 saveRa0 = sc->RA0M;
    dsp_dma03(ScuDsp, sc->dsp_dma_instruction);
    sc->RA0 = saveRa0;
  }
  else if (((sc->dsp_dma_instruction >> 10) & 0x1F) == 0x1C)
  {
    u32 saveWa0 = sc->WA0M;
    dsp_dma04(ScuDsp, sc->dsp_dma_instruction);
    sc->WA0 = saveWa0;
  }

  sc->ProgControlPort.part.T0 = 0;
  sc->dsp_dma_instruction = 0;
  sc->dsp_dma_wait = 0;

}


void ScuTimer1Exec( u32 timing ) {
  if (ScuRegs->timer1_counter > 0) {
    ScuRegs->timer1_counter = (ScuRegs->timer1_counter - (timing >> 1));
    if (ScuRegs->timer1_counter <= 0) {
      ScuRegs->timer1_set = 1;
      if ((ScuRegs->T1MD & 0x80) == 0 || ScuRegs->timer0_set == 1) {
        ScuSendTimer1();
      }
    }
  }
}

void ScuSetAddValue(scudmainfo_struct * dmainfo) {
	dmainfo->ReadAdd = (dmainfo->AddValue & 0x100) >> 6;
	dmainfo->WriteAdd = 1 << (dmainfo->AddValue & 0x7);

  if (dmainfo->ModeAddressUpdate & 0x1000000) {
    dmainfo->InDirectAdress = dmainfo->WriteAddress;
    ReadFunc32 read32_func = mem_read32_arr[MEM_GET_FUNC_ADDR(dmainfo->InDirectAdress)];

    dmainfo->TransferNumber = read32_func(dmainfo->InDirectAdress);
    dmainfo->WriteAddress = read32_func(dmainfo->InDirectAdress + 4);
    dmainfo->ReadAddress = read32_func(dmainfo->InDirectAdress + 8);
    dmainfo->InDirectAdress += 0xC;
  }
  else {
    if (dmainfo->mode > 0) {
      dmainfo->TransferNumber &= 0xFFF;
      if (dmainfo->TransferNumber == 0)
        dmainfo->TransferNumber = 0x1000;
    }
    else {
      if (dmainfo->TransferNumber == 0)
        dmainfo->TransferNumber = 0x100000;
    }
  }

  //LOG("[SCU] Run DMA src=%08X,dst=%08X,size=%d, ra:%d/wa:%d flame=%d:%d",
  //  dmainfo->ReadAddress, dmainfo->WriteAddress, dmainfo->TransferNumber,
  //  dmainfo->ReadAdd, dmainfo->WriteAdd, yabsys.frame_count, yabsys.LineCount);

}

void SucDmaExec(scudmainfo_struct * dma, int * time ) {
  //LOG("[SCU] SucDmaExec src=%08X,dst=%08X,size=%d, ra:%d/wa:%d flame=%d:%d",
   // dma->ReadAddress, dma->WriteAddress, dma->TransferNumber, dma->ReadAdd, dma->WriteAdd, yabsys.frame_count, yabsys.LineCount);
  //u32 cycle = 0;

  if (dma->ReadAdd == 0) {
    // DMA fill
    // Is it a constant source or a register whose value can change from
    // read to read?
    int constant_source = ((dma->ReadAddress & 0x1FF00000) == 0x00200000)
      || ((dma->ReadAddress & 0x1E000000) == 0x06000000)
      || ((dma->ReadAddress & 0x1FF00000) == 0x05A00000)
      || ((dma->ReadAddress & 0x1DF00000) == 0x05C00000);

    if ((dma->WriteAddress & 0x1FFFFFFF) >= 0x5A00000
      && (dma->WriteAddress & 0x1FFFFFFF) < 0x5FF0000) {
      // Fill a 32-bit value in 16-bit units.  We have to be careful to
      // avoid misaligned 32-bit accesses, because some hardware (e.g.
      // PSP) crashes on such accesses.
      if (constant_source) {
        u32 val;
        if (dma->ReadAddress & 2) {  // Avoid misaligned access
          val = mem_read16_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]( (dma->ReadAddress&0x0FFFFFFF)) << 16
            | mem_read16_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]( (dma->ReadAddress&0x0FFFFFFF) + 2);
        }
        else {
          val = mem_read32_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]((dma->ReadAddress & 0x0FFFFFFF));
        }

        u32 start = dma->WriteAddress;
        while ( *time > 0 ) {
          *time -= 1;
          mem_write16_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, (u16)(val >> 16));
          dma->WriteAddress += dma->WriteAdd;
          mem_write16_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, (u16)val);
          dma->WriteAddress += dma->WriteAdd;
          dma->TransferNumber -= 4;
          if (dma->TransferNumber <= 0 ) {
            SH2WriteNotify(start, dma->WriteAddress - start);
            return;
          }
        }
        SH2WriteNotify(start, dma->WriteAddress - start);
      }
      else {
        u32 start = dma->WriteAddress;
        while ( *time > 0) {
          *time -= 1;
          u32 tmp = mem_read32_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]((dma->ReadAddress & 0x0FFFFFFF));
          mem_write16_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, (u16)(tmp >> 16));
          dma->WriteAddress += dma->WriteAdd;
          mem_write16_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, (u16)tmp);
          dma->WriteAddress += dma->WriteAdd;
          dma->ReadAddress += dma->ReadAdd;
          dma->TransferNumber -= 4;
          if (dma->TransferNumber <= 0) {
            SH2WriteNotify(start, dma->WriteAddress - start);
            return;
          }
        }
        SH2WriteNotify(start, dma->WriteAddress - start);
      }
    }
    else {
      // Fill in 32-bit units (always aligned).
      u32 start = dma->WriteAddress;
      if (constant_source) {
        u32 val = mem_read32_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]((dma->ReadAddress & 0x0FFFFFFF));
        while ( *time > 0) {
          *time -= 1;
          mem_write32_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, val);
          dma->ReadAddress += dma->ReadAdd;
          dma->WriteAddress += dma->WriteAdd;
          dma->TransferNumber -= 4;
          if (dma->TransferNumber <= 0) {
            SH2WriteNotify(start, dma->WriteAddress - start);
            return;
          }
        }
      }
      else {
        while (*time > 0) {
          *time -= 1;
          u32 val = mem_read32_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]((dma->ReadAddress & 0x0FFFFFFF));
          mem_write32_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, val);
          dma->ReadAddress += dma->ReadAdd;
          dma->WriteAddress += dma->WriteAdd;
          dma->TransferNumber -= 4;
          if (dma->TransferNumber <= 0) {
            SH2WriteNotify(start, dma->WriteAddress - start);
            return;
          }
        }
      }
      // Inform the SH-2 core in case it was a write to main RAM.
      SH2WriteNotify(start, dma->WriteAddress - start);
    }

  } else {
    // DMA copy
    // Access to B-BUS?
    if (((dma->WriteAddress & 0x1FFFFFFF) >= 0x5A00000 && (dma->WriteAddress & 0x1FFFFFFF) < 0x5FF0000)) {
      // Copy in 16-bit units, avoiding misaligned accesses.
      //u32 counter = 0;
      u32 start = dma->WriteAddress;
      while (*time > 0) {
        *time -= 1;
        u16 tmp = mem_read16_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]((dma->ReadAddress & 0x0FFFFFFF));
        mem_write16_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, tmp);
        dma->WriteAddress += dma->WriteAdd;
        dma->ReadAddress += 2;
        dma->TransferNumber -= 2;
        if (dma->TransferNumber <= 0) {
          SH2WriteNotify(start, dma->WriteAddress - start);
          return;
        }
      }
      SH2WriteNotify(start, dma->WriteAddress - start);
    }
    else if (((dma->ReadAddress & 0x1FFFFFFF) >= 0x5A00000 && (dma->ReadAddress & 0x1FFFFFFF) < 0x5FF0000)) {
      u32 start = dma->WriteAddress;
      while ( *time > 0) {
        *time -= 1;
        u16 tmp = mem_read16_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]((dma->ReadAddress & 0x0FFFFFFF));
        mem_write16_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, tmp);
        dma->WriteAddress += (dma->WriteAdd >> 1);
        dma->ReadAddress += 2;
        dma->TransferNumber -= 2;
        if (dma->TransferNumber <= 0) {
          SH2WriteNotify(start, dma->WriteAddress - start);
          return;
        }
      }
      SH2WriteNotify(start, dma->WriteAddress - start);
    }
    else {
      //u32 counter = 0;
      u32 start = dma->WriteAddress;
      while (*time > 0) {
        *time -= 1;
        u32 val = mem_read32_arr[MEM_GET_FUNC_ADDR(dma->ReadAddress & 0x0FFFFFFF)]((dma->ReadAddress & 0x0FFFFFFF));
        mem_write32_arr[MEM_GET_FUNC_ADDR(dma->WriteAddress & 0x0FFFFFFF)](dma->WriteAddress, val);
        dma->ReadAddress += 4;
        dma->WriteAddress += dma->WriteAdd;
        dma->TransferNumber -= 4;
        if (dma->TransferNumber <= 0) {
          SH2WriteNotify(start, dma->WriteAddress - start);
          return;
        }
      }
      /* Inform the SH-2 core in case it was a write to main RAM */
      SH2WriteNotify(start, dma->WriteAddress - start);
    }

  }  // Fill / copy


}


void SucDmaCheck(scudmainfo_struct * dma, int time) {
  int atime = time;
  if (dma->TransferNumber > 0) {
    if (dma->ModeAddressUpdate & 0x1000000) {
	  ReadFunc32 read32_func = mem_read32_arr[MEM_GET_FUNC_ADDR(dma->InDirectAdress)];
      while (atime > 0) {
        SucDmaExec(dma, &atime);
        if (dma->TransferNumber <= 0) {
          if (dma->ReadAddress & 0x80000000) {
            ScuSendDMAEnd(dma->mode);
            dma->TransferNumber = 0;
            return;
          }
          else {
            dma->TransferNumber = read32_func(dma->InDirectAdress);
            dma->WriteAddress = read32_func(dma->InDirectAdress + 4);
            dma->ReadAddress = read32_func(dma->InDirectAdress + 8);
            dma->InDirectAdress += 0xC;
          }
        }
      }

    }
    else {
      SucDmaExec(dma, &atime);
      if (dma->TransferNumber <= 0) {
        ScuSendDMAEnd(dma->mode);
      }
    }
  }
  return;
}


void ScuDmaProc(Scu * scu, int time) {
	SucDmaCheck(&scu->dma0, time);
	SucDmaCheck(&scu->dma1, time);
	SucDmaCheck(&scu->dma2, time);
}

//////////////////////////////////////////////////////////////////////////////
void ScuExec(u32 timing) {
   int i;

   if ( ScuRegs->T1MD & 0x1 ){
     if ( (ScuRegs->T1MD & 0x80) == 0) {
       ScuTimer1Exec(timing);
     }
     else {
       if (yabsys.LineCount == ScuRegs->T0C || ScuRegs->T0C > 500 ) {
         ScuTimer1Exec(timing);
       }
     }
   }

  ScuDmaProc(ScuRegs, (int)timing<<4);

   // is dsp executing?
   if (ScuDsp->ProgControlPort.part.EX) {


     s32 dsp_counter = (s32)timing;
      while (dsp_counter > 0) {
         u32 instruction;

         // Make sure it isn't one of our breakpoints
         for (i=0; i < ScuBP->numcodebreakpoints; i++) {
            if ((ScuDsp->PC == ScuBP->codebreakpoint[i].addr) && ScuBP->inbreakpoint == 0) {
               ScuBP->inbreakpoint = 1;
               if (ScuBP->BreakpointCallBack) ScuBP->BreakpointCallBack(ScuBP->codebreakpoint[i].addr);
                 ScuBP->inbreakpoint = 0;
            }
         }

         if (ScuDsp->ProgControlPort.part.T0 != 0) {
           step_dsp_dma(ScuDsp);
         }

         instruction = ScuDsp->ProgramRam[ScuDsp->PC];
         //LOG("scu: dsp %08X @ %08X", instruction, ScuDsp->PC);
         incFlg[0] = 0;
         incFlg[1] = 0;
         incFlg[2] = 0;
         incFlg[3] = 0;

         ScuDsp->ALU.all = ScuDsp->AC.all;
         // ALU commands
         switch (instruction >> 26)
         {
            case 0x0: // NOP
               //AC is moved as-is to the ALU
              //ScuDsp->ALU.all = ScuDsp->AC.all;
               break;
            case 0x1: // AND
               //the upper 16 bits of AC are not modified for and, or, add, sub, rr and rl8
              ScuDsp->ALU.part.L = (s64)((u32)ScuDsp->AC.part.L & (u32)ScuDsp->P.part.L);

               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
               ScuDsp->ProgControlPort.part.S = ((s64)ScuDsp->ALU.part.L < 0);
               ScuDsp->ProgControlPort.part.C = 0;
               break;
            case 0x2: // OR
               ScuDsp->ALU.part.L = (u64)((u32)ScuDsp->AC.part.L | (u32)ScuDsp->P.part.L);

               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
               ScuDsp->ProgControlPort.part.S = ((s64)ScuDsp->ALU.part.L < 0);
               ScuDsp->ProgControlPort.part.C = 0;
               break;
            case 0x3: // XOR
              ScuDsp->ALU.part.L = (u64)((u32)ScuDsp->AC.part.L ^ (u32)ScuDsp->P.part.L);

               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
               ScuDsp->ProgControlPort.part.S = ((s64)ScuDsp->ALU.part.L < 0);
               ScuDsp->ProgControlPort.part.C = 0;
               break;
            case 0x4: // ADD
               ScuDsp->ALU.part.L = (s32)ScuDsp->AC.part.L + (s32)ScuDsp->P.part.L;
               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
               ScuDsp->ProgControlPort.part.S = ((s64)ScuDsp->ALU.part.L < 0);

               //0x00000001 + 0xFFFFFFFF will set the carry bit, needs to be unsigned math
               if (((u64)(u32)ScuDsp->P.part.L + (u64)(u32)ScuDsp->AC.part.L) & 0x100000000){
                 ScuDsp->ProgControlPort.part.C = 1;
               }
               else{
                 ScuDsp->ProgControlPort.part.C = 0;
               }


               //if (ScuDsp->ALU.part.L ??) // set overflow flag
               //    ScuDsp->ProgControlPort.part.V = 1;
               //else
               //   ScuDsp->ProgControlPort.part.V = 0;
               break;
            case 0x5: // SUB
            {
              ScuDsp->ALU.part.L = (s32)ScuDsp->AC.part.L - (s32)ScuDsp->P.part.L;
              //ScuDsp->ProgControlPort.part.C = ((ans >> 32) & 0x01);

              //ScuDsp->ALU.part.L = ans;

               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
               ScuDsp->ProgControlPort.part.S = ((s64)ScuDsp->ALU.part.L < 0);

              //0x00000001 - 0xFFFFFFFF will set the carry bit, needs to be unsigned math
              if ((((u64)(u32)ScuDsp->AC.part.L - (u64)(u32)ScuDsp->P.part.L)) & 0x100000000)
                ScuDsp->ProgControlPort.part.C = 1;
              else
                ScuDsp->ProgControlPort.part.C = 0;

              //0x00000001 - 0xFFFFFFFF will set the carry bit, needs to be unsigned math
              //if ((((u64)(u32)ScuDsp->AC.part.L - (u64)(u32)ScuDsp->P.part.L)) & 0x100000000)
              //  ScuDsp->ProgControlPort.part.C = 1;
              //else
              //  ScuDsp->ProgControlPort.part.C = 0;


              //               if (ScuDsp->ALU.part.L ??) // set overflow flag
              //                  ScuDsp->ProgControlPort.part.V = 1;
              //               else
              //                  ScuDsp->ProgControlPort.part.V = 0;
            }
               break;
            case 0x6: // AD2
              ScuDsp->ALU.all = (s64)ScuDsp->AC.all +(s64)ScuDsp->P.all;
               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.all == 0);

               //0x500000000000 + 0xd00000000000 will set the sign bit
               if (ScuDsp->ALU.all & 0x800000000000)
                  ScuDsp->ProgControlPort.part.S = 1;
               else
                  ScuDsp->ProgControlPort.part.S = 0;

               //AC.all and P.all are sign-extended so we need to mask it off and check for a carry
               if (((ScuDsp->AC.all & 0xffffffffffff) + (ScuDsp->P.all & 0xffffffffffff)) & (0x1000000000000))
                  ScuDsp->ProgControlPort.part.C = 1;
               else
                  ScuDsp->ProgControlPort.part.C = 0;

//               if (ScuDsp->ALU.part.unused != 0)
//                  ScuDsp->ProgControlPort.part.V = 1;
//               else
//                  ScuDsp->ProgControlPort.part.V = 0;

               break;
            case 0x8: // SR
			   ScuDsp->ProgControlPort.part.C = ScuDsp->AC.part.L & 0x1;
               ScuDsp->ALU.part.L = (ScuDsp->AC.part.L & 0x80000000) | (ScuDsp->AC.part.L >> 1);
               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
               ScuDsp->ProgControlPort.part.S = ((u32)ScuDsp->ALU.part.L >> 31);

               //0x00000001 >> 1 will set the carry bit
               //ScuDsp->ProgControlPort.part.C = ScuDsp->ALU.part.L >> 31; would not handle this case
               break;
            case 0x9: // RR
              ScuDsp->ProgControlPort.part.C = ScuDsp->AC.part.L & 0x1;
               ScuDsp->ALU.part.L = ((u32)(ScuDsp->ProgControlPort.part.C) << 31) | ((u32)(ScuDsp->AC.part.L) >> 1) ;

               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
			   ScuDsp->ProgControlPort.part.S = ((u32)ScuDsp->ALU.part.L >> 31);
               break;
            case 0xA: // SL
              ScuDsp->ProgControlPort.part.C = (ScuDsp->AC.part.L >> 31) & 0x01;

               ScuDsp->ALU.part.L = (u32)(ScuDsp->AC.part.L << 1);
               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
			   ScuDsp->ProgControlPort.part.S = ((u32)ScuDsp->ALU.part.L >> 31);
               break;
            case 0xB: // RL

              ScuDsp->ProgControlPort.part.C = (ScuDsp->AC.part.L >> 31) & 0x01;

               ScuDsp->ALU.part.L = (((u32)ScuDsp->AC.part.L << 1) | ScuDsp->ProgControlPort.part.C);
               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
			   ScuDsp->ProgControlPort.part.S = ((u32)ScuDsp->ALU.part.L >> 31);

               //ScuDsp->AC.part.L = ScuDsp->ALU.part.L;
               break;
            case 0xF: // RL8
              ScuDsp->ProgControlPort.part.C = (ScuDsp->AC.part.L >> 24) & 0x01;
              ScuDsp->ALU.part.L  = ((u32)(ScuDsp->AC.part.L << 8) | ((ScuDsp->AC.part.L >> 24) & 0xFF)) ;
               ScuDsp->ProgControlPort.part.Z = (ScuDsp->ALU.part.L == 0);
			   ScuDsp->ProgControlPort.part.S = ((u32)ScuDsp->ALU.part.L >> 31);

               //rotating 0xff000000 left 8 will produce 0x000000ff and set the
               //carry bit
               //ScuDsp->ProgControlPort.part.C = (ScuDsp->AC.part.L >> 24) & 0x01;
               break;
            default: break;
         }


         switch (instruction >> 30) {
         case 0x00: // Operation Commands
               switch ((instruction >> 23) & 0x3)
               {
                  case 2: // MOV MUL, P
                    ScuDsp->P.all = (s64)ScuDsp->RX * (s32)ScuDsp->RY; // ScuDsp->MUL.all;
                     break;
                  case 3: // MOV [s], P
                     //s32 cast to sign extend
                     ScuDsp->P.all = (s64)(s32)readgensrc((instruction >> 20) & 0x7);
                     break;
                  default: break;
               }
               // X-bus
               if ((instruction >> 23) & 0x4)
               {
                 // MOV [s], X
                 ScuDsp->RX = readgensrc((instruction >> 20) & 0x7);
               }

               // Y-bus
               if ((instruction >> 17) & 0x4)
               {
                  // MOV [s], Y
                  ScuDsp->RY = readgensrc((instruction >> 14) & 0x7);
               }
               switch ((instruction >> 17) & 0x3)
               {
                  case 1: // CLR A
                     ScuDsp->AC.all = 0;
                     break;
                  case 2: // MOV ALU,A
                     ScuDsp->AC.all = ScuDsp->ALU.all;
                     break;
                  case 3: // MOV [s],A
                     //s32 cast to sign extend
                     ScuDsp->AC.all = (s64)(s32)readgensrc((instruction >> 14) & 0x7);
                     break;
                  default: break;
               }


               // D1-bus
               switch ((instruction >> 12) & 0x3)
               {
                  case 1: // MOV SImm,[d]
					//Note: incFlg is binary
					ScuDsp->CT[0] = (ScuDsp->CT[0] + incFlg[0]) & 0x3f; incFlg[0] = 0;
					ScuDsp->CT[1] = (ScuDsp->CT[1] + incFlg[1]) & 0x3f; incFlg[1] = 0;
					ScuDsp->CT[2] = (ScuDsp->CT[2] + incFlg[2]) & 0x3f; incFlg[2] = 0;
					ScuDsp->CT[3] = (ScuDsp->CT[3] + incFlg[3]) & 0x3f; incFlg[3] = 0;
                     writed1busdest((instruction >> 8) & 0xF, (u32)(signed char)(instruction & 0xFF));
                     break;
                  case 3: // MOV [s],[d]
                     writed1busdest((instruction >> 8) & 0xF, readgensrc(instruction & 0xF));
                     break;
                  default: break;
               }

               break;
            case 0x02: // Load Immediate Commands
               if ((instruction >> 25) & 1)
               {
                  switch ((instruction >> 19) & 0x3F) {
                     case 0x01: // MVI Imm,[d]NZ
                        if (!ScuDsp->ProgControlPort.part.Z)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x02: // MVI Imm,[d]NS
                        if (!ScuDsp->ProgControlPort.part.S)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x03: // MVI Imm,[d]NZS
                        if ( ScuDsp->ProgControlPort.part.Z == 0 && ScuDsp->ProgControlPort.part.S == 0)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x04: // MVI Imm,[d]NC
                        if (!ScuDsp->ProgControlPort.part.C)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x08: // MVI Imm,[d]NT0
                        if (!ScuDsp->ProgControlPort.part.T0)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x21: // MVI Imm,[d]Z
                        if (ScuDsp->ProgControlPort.part.Z)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x22: // MVI Imm,[d]S
                        if (ScuDsp->ProgControlPort.part.S)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x23: // MVI Imm,[d]ZS
                        if (ScuDsp->ProgControlPort.part.Z || ScuDsp->ProgControlPort.part.S)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x24: // MVI Imm,[d]C
                        if (ScuDsp->ProgControlPort.part.C)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     case 0x28: // MVI Imm,[d]T0
                        if (ScuDsp->ProgControlPort.part.T0)
                           writeloadimdest((instruction >> 26) & 0xF, (instruction & 0x7FFFF) | (-(instruction & 0x40000)));
                        break;
                     default: break;
                  }
               }
               else
               {
                  // MVI Imm,[d]
                  int value = (instruction & 0x1FFFFFF) | (-(instruction & 0x1000000));
                  writeloadimdest((instruction >> 26) & 0xF, value);
                }
               break;
            case 0x03: // Other
            {
               switch((instruction >> 28) & 0xF) {
                 case 0x0C: // DMA Commands
                 {
                   // Finish Previous DMA operation
                   if (ScuDsp->dsp_dma_wait > 0) {
                     ScuDsp->dsp_dma_wait = 0;
                     step_dsp_dma(ScuDsp);
                   }

                   ScuDsp->dsp_dma_instruction = instruction;
                   ScuDsp->ProgControlPort.part.T0 = 1;

                   int Counter = 0;
                   if ( ((instruction >> 10) & 0x1F) == 0x00 ||
                        ((instruction >> 10) & 0x1F) == 0x04  ||
                        ((instruction >> 11) & 0x0F) == 0x08 ||
                        ((instruction >> 10) & 0x1F) == 0x14 )
                   {
                      Counter = instruction & 0xFF;
                   }
                   else if (
                     ((instruction >> 11) & 0x0F) == 0x04 ||
                     ((instruction >> 10) & 0x1F) == 0x0C ||
                     ((instruction >> 11) & 0x0F) == 0x0C ||
                     ((instruction >> 10) & 0x1F) == 0x1C)
                   {
					 u32 val = (instruction & 0x3);
					 Counter = ScuDsp->MD[val][ScuDsp->CT[val] & 0x3F];
					 //if ((instruction >> 2) & 1)) {
					 //XXX: check if ScuDsp->CT[val] is in 0x3F always or where else does it change
					 if (instruction & 4) {
						 ScuDsp->CT[val] = (ScuDsp->CT[val] + 1) & 0x3F;
				     }

                   }

                   ScuDsp->dsp_dma_size = Counter;
                   ScuDsp->dsp_dma_wait = 2; // DMA operation will be start when this count is zero
                   ScuDsp->WA0M = ScuDsp->WA0;
                   ScuDsp->RA0M = ScuDsp->RA0;

                   int cycle = 0;
                   switch ((ScuDsp->WA0M << 2) & 0xDFF00000) {
                   case 0x00200000: /* Low */
                     cycle = 2;
                     break;
                   case 0x05A00000: /* SOUND */
                     cycle = 1;
                     break;
                   case 0x05C00000: /* VDP1 */
                     cycle = 1;
                     break;
                   case 0x05e00000: /* VDP2 */
                     cycle = 1;
                     break;
                   case 0x06000000: /* High */
                     cycle = 4;
                     break;
                   default:
                     cycle = 4;
                   }
                   ScuDsp->dsp_dma_wait = (Counter >> cycle) + 1;
                   //LOG("Start DSP DMA RA=%08X WA=%08X inst=%08X count=%d wait = %d", ScuDsp->RA0M<<2, ScuDsp->WA0M<<2, ScuDsp->dsp_dma_instruction, Counter, ScuDsp->dsp_dma_wait );
                   break;
                  }
                  case 0x0D: // Jump Commands
                    if (ScuDsp->jmpaddr != 0xffffffff) {
                      break;
                    }
                     switch ((instruction >> 19) & 0x7F) {
                        case 0x00: // JMP Imm
                           ScuDsp->jmpaddr = instruction & 0xFF;
                           ScuDsp->delayed = 0;
                           break;
                        case 0x41: // JMP NZ, Imm
                           if (!ScuDsp->ProgControlPort.part.Z)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }
                           break;
                        case 0x42: // JMP NS, Imm
                           if (!ScuDsp->ProgControlPort.part.S)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }

                           //LOG("scu\t: JMP NS: S = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x43: // JMP NZS, Imm
                           if ( ScuDsp->ProgControlPort.part.Z==0 && ScuDsp->ProgControlPort.part.S == 0)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }

                           //LOG("scu\t: JMP NZS: Z = %d, S = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.Z, (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x44: // JMP NC, Imm
                           if (!ScuDsp->ProgControlPort.part.C)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }
                           break;
                        case 0x48: // JMP NT0, Imm
                           if (!ScuDsp->ProgControlPort.part.T0)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }

                           //LOG("scu\t: JMP NT0: T0 = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.T0, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x61: // JMP Z,Imm
                           if (ScuDsp->ProgControlPort.part.Z)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }
                           break;
                        case 0x62: // JMP S, Imm
                           if (ScuDsp->ProgControlPort.part.S)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }

                           //LOG("scu\t: JMP S: S = %d, jmpaddr = %08X\n", (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x63: // JMP ZS, Imm
                           if (ScuDsp->ProgControlPort.part.Z || ScuDsp->ProgControlPort.part.S)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }

                           //LOG("scu\t: JMP ZS: Z = %d, S = %d, jmpaddr = %08X\n", ScuDsp->ProgControlPort.part.Z, (unsigned int)ScuDsp->ProgControlPort.part.S, (unsigned int)ScuDsp->jmpaddr);
                           break;
                        case 0x64: // JMP C, Imm
                           if (ScuDsp->ProgControlPort.part.C)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }
                           break;
                        case 0x68: // JMP T0,Imm
                           if (ScuDsp->ProgControlPort.part.T0)
                           {
                              ScuDsp->jmpaddr = instruction & 0xFF;
                              ScuDsp->delayed = 0;
                           }
                           break;
                        default:
                           LOG("scu\t: Unknown JMP instruction not implemented\n");
                           break;
                     }
                     break;
                  case 0x0E: // Loop bottom Commands
                     if (instruction & 0x8000000)
                     {
                        // LPS
                        if (ScuDsp->LOP != 0)
                        {
                           ScuDsp->jmpaddr = ScuDsp->PC;
                           ScuDsp->delayed = 0;
                           ScuDsp->LOP--;
                        }
                     }
                     else
                     {
                        // BTM
                        if (ScuDsp->LOP != 0)
                        {
                           ScuDsp->jmpaddr = ScuDsp->TOP;
                           ScuDsp->delayed = 0;
                           ScuDsp->LOP--;
                        }
                     }

                     break;
                  case 0x0F: // End Commands
                     ScuDsp->ProgControlPort.part.EX = 0;

                     if (instruction & 0x8000000) {
                        // End with Interrupt
                        ScuDsp->ProgControlPort.part.E = 1;
                        ScuSendDSPEnd();
                     }

                     LOG("dsp has ended\n");
                     ScuDsp->ProgControlPort.part.P = ScuDsp->PC+1;
                     dsp_counter = 1;
                     break;
                  default: break;
               }
               break;
            }
            default:
               LOG("scu\t: Invalid DSP opcode %08X at offset %02X\n", instruction, ScuDsp->PC);
               break;
         }

         //ScuDsp->MUL.all = (s64)ScuDsp->RX * (s32)ScuDsp->RY;
		//Note: incFlg is binary
		ScuDsp->CT[0] = (ScuDsp->CT[0] + incFlg[0]) & 0x3f; incFlg[0] = 0;
		ScuDsp->CT[1] = (ScuDsp->CT[1] + incFlg[1]) & 0x3f; incFlg[1] = 0;
		ScuDsp->CT[2] = (ScuDsp->CT[2] + incFlg[2]) & 0x3f; incFlg[2] = 0;
		ScuDsp->CT[3] = (ScuDsp->CT[3] + incFlg[3]) & 0x3f; incFlg[3] = 0;

         ScuDsp->PC++;

         // Handle delayed jumps
         if (ScuDsp->jmpaddr != 0xFFFFFFFF)
         {
            if (ScuDsp->delayed)
            {
               ScuDsp->PC = (unsigned char)ScuDsp->jmpaddr;
               ScuDsp->jmpaddr = 0xFFFFFFFF;
               dsp_counter += 1; // hold clock
            }
            else
               ScuDsp->delayed = 1;
         }
         dsp_counter--;
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

void ScuDspGetRegisters(scudspregs_struct *regs) {
   if (regs != NULL) {
      memcpy(regs->ProgramRam, ScuDsp->ProgramRam, sizeof(u32) * 256);
      memcpy(regs->MD, ScuDsp->MD, sizeof(u32) * 64 * 4);

      regs->ProgControlPort.all = ScuDsp->ProgControlPort.all;
      regs->ProgControlPort.part.P = regs->PC = ScuDsp->PC;
      regs->TOP = ScuDsp->TOP;
      regs->LOP = ScuDsp->LOP;
      regs->jmpaddr = ScuDsp->jmpaddr;
      regs->delayed = ScuDsp->delayed;
      regs->DataRamPage = ScuDsp->DataRamPage;
      regs->DataRamReadAddress = ScuDsp->DataRamReadAddress;
      memcpy(regs->CT, ScuDsp->CT, sizeof(u8) * 4);
      regs->RX = ScuDsp->RX;
      regs->RY = ScuDsp->RY;
      regs->RA0 = ScuDsp->RA0;
      regs->WA0 = ScuDsp->WA0;

      regs->AC.all = ScuDsp->AC.all;
      regs->P.all = ScuDsp->P.all;
      regs->ALU.all = ScuDsp->ALU.all;
      regs->MUL.all = ScuDsp->MUL.all;
   }
}

//////////////////////////////////////////////////////////////////////////////

void ScuDspSetRegisters(scudspregs_struct *regs) {
   if (regs != NULL) {
      memcpy(ScuDsp->ProgramRam, regs->ProgramRam, sizeof(u32) * 256);
      memcpy(ScuDsp->MD, regs->MD, sizeof(u32) * 64 * 4);

      ScuDsp->ProgControlPort.all = regs->ProgControlPort.all;
      ScuDsp->PC = regs->ProgControlPort.part.P;
      ScuDsp->TOP = regs->TOP;
      ScuDsp->LOP = regs->LOP;
      ScuDsp->jmpaddr = regs->jmpaddr;
      ScuDsp->delayed = regs->delayed;
      ScuDsp->DataRamPage = regs->DataRamPage;
      ScuDsp->DataRamReadAddress = regs->DataRamReadAddress;
      memcpy(ScuDsp->CT, regs->CT, sizeof(u8) * 4);
      ScuDsp->RX = regs->RX;
      ScuDsp->RY = regs->RY;
      ScuDsp->RA0 = regs->RA0;
      ScuDsp->WA0 = regs->WA0;

      ScuDsp->AC.all = regs->AC.all;
      ScuDsp->P.all = regs->P.all;
      ScuDsp->ALU.all = regs->ALU.all;
      ScuDsp->MUL.all = regs->MUL.all;
   }
}

//////////////////////////////////////////////////////////////////////////////

void ScuDspSetBreakpointCallBack(void (*func)(u32)) {
   ScuBP->BreakpointCallBack = func;
}

//////////////////////////////////////////////////////////////////////////////

int ScuDspAddCodeBreakpoint(u32 addr) {
   int i;

   if (ScuBP->numcodebreakpoints < MAX_BREAKPOINTS) {
      // Make sure it isn't already on the list
      for (i = 0; i < ScuBP->numcodebreakpoints; i++)
      {
         if (addr == ScuBP->codebreakpoint[i].addr)
            return -1;
      }

      ScuBP->codebreakpoint[ScuBP->numcodebreakpoints].addr = addr;
      ScuBP->numcodebreakpoints++;

      return 0;
   }

   return -1;
}

//////////////////////////////////////////////////////////////////////////////

static void ScuDspSortCodeBreakpoints(void) {
   int i, i2;
   u32 tmp;

   for (i = 0; i < (MAX_BREAKPOINTS-1); i++)
   {
      for (i2 = i+1; i2 < MAX_BREAKPOINTS; i2++)
      {
         if (ScuBP->codebreakpoint[i].addr == 0xFFFFFFFF &&
            ScuBP->codebreakpoint[i2].addr != 0xFFFFFFFF)
         {
            tmp = ScuBP->codebreakpoint[i].addr;
            ScuBP->codebreakpoint[i].addr = ScuBP->codebreakpoint[i2].addr;
            ScuBP->codebreakpoint[i2].addr = tmp;
         }
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

int ScuDspDelCodeBreakpoint(u32 addr) {
   int i;

   if (ScuBP->numcodebreakpoints > 0) {
      for (i = 0; i < ScuBP->numcodebreakpoints; i++) {
         if (ScuBP->codebreakpoint[i].addr == addr)
         {
            ScuBP->codebreakpoint[i].addr = 0xFFFFFFFF;
            ScuDspSortCodeBreakpoints();
            ScuBP->numcodebreakpoints--;
            return 0;
         }
      }
   }

   return -1;
}

//////////////////////////////////////////////////////////////////////////////

scucodebreakpoint_struct *ScuDspGetBreakpointList(void) {
   return ScuBP->codebreakpoint;
}

//////////////////////////////////////////////////////////////////////////////

void ScuDspClearCodeBreakpoints(void) {
   int i;
   for (i = 0; i < MAX_BREAKPOINTS; i++)
      ScuBP->codebreakpoint[i].addr = 0xFFFFFFFF;

   ScuBP->numcodebreakpoints = 0;
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL ScuReadByte(u32 addr) {
   addr &= 0xFF;

   switch(addr) {
      case 0xA7:
         return (ScuRegs->IST & 0xFF);
      default:
         LOG("Unhandled SCU Register byte read %08X\n", addr);
         return 0;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL ScuReadWord(u32 addr) {
   addr &= 0xFF;
   LOG("Unhandled SCU Register word read %08X\n", addr);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL ScuReadLong(u32 addr) {
   addr &= 0xFF;
   //LOG("scu: read  %08X @ %08X", addr, CurrentSH2->regs.PC);
   switch(addr) {
      case 0:
         return ScuRegs->D0R;
      case 4:
         return ScuRegs->D0W;
      case 8:
         return ScuRegs->D0C;
      case 0x20:
         return ScuRegs->D1R;
      case 0x24:
         return ScuRegs->D1W;
      case 0x28:
         return ScuRegs->D1C;
      case 0x40:
         return ScuRegs->D2R;
      case 0x44:
         return ScuRegs->D2W;
      case 0x48:
         return ScuRegs->D2C;
      case 0x7C: {
        if (ScuRegs->dma0.TransferNumber > 0) { ScuRegs->DSTA |= 0x10; }else{ ScuRegs->DSTA &= ~0x10;  }
        if (ScuRegs->dma1.TransferNumber > 0) { ScuRegs->DSTA |= 0x100; }else{ ScuRegs->DSTA &= ~0x100;  }
        if (ScuRegs->dma2.TransferNumber > 0) { ScuRegs->DSTA |= 0x1000; }else{ ScuRegs->DSTA &= ~0x1000; }
        return ScuRegs->DSTA;
      }
      case 0x80: // DSP Program Control Port
         return (ScuDsp->ProgControlPort.all & 0x00FD00FF);
      case 0x8C: // DSP Data Ram Data Port
         if (!ScuDsp->ProgControlPort.part.EX){
            u32 rtn = ScuDsp->MD[ (ScuDsp->DataRamReadAddress >> 6) & 0x3][(ScuDsp->DataRamReadAddress) & 0x3F ];
            ScuDsp->DataRamReadAddress++;
            return rtn;
         }else
            return 0;
      case 0xA4:
         //LOG("Read IST %08X", ScuRegs->IST);
         return ScuRegs->IST;
      case 0xA8:
         return ScuRegs->AIACK;
      case 0xC4:
         return ScuRegs->RSEL;
      case 0xC8:
         return ScuRegs->VER;
      default:
         LOG("Unhandled SCU Register long read %08X\n", addr);
         return 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL ScuWriteByte(u32 addr, u8 val) {
   addr &= 0xFF;
   switch(addr) {
      case 0xA7:
      {
        u32 after = ScuRegs->IST & (0xFFFFFF00 | val);
        //LOG("IST = from %X to %X PC=%X frame=%d:%d", ScuRegs->IST, after, CurrentSH2->regs.PC, yabsys.frame_count, yabsys.LineCount);
        ScuRemoveInterruptByCPU(ScuRegs->IST, after);
        ScuRegs->IST = after; // double check this
        ScuTestInterruptMask();
      }
         return;
      default:
         LOG("Unhandled SCU Register byte write %08X\n", addr);
         return;
   }
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL ScuWriteWord(u32 addr, UNUSED u16 val) {
   //addr &= 0xFF;
  // LOG("Unhandled SCU Register word write %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL ScuWriteLong(u32 addr, u32 val) {
   addr &= 0xFF;
  //if (addr!= 0xA0)
  //LOG("scu: write %08X:%08X @ %08X", addr, val, CurrentSH2->regs.PC);
   switch(addr) {
      case 0:
         ScuRegs->D0R = val;
         break;
      case 4:
         ScuRegs->D0W = val;
         break;
      case 8:
         ScuRegs->D0C = val;
         break;
      case 0xC:
         ScuRegs->D0AD = val;
         break;
      case 0x10:
      if ((val & 0x1) && ((ScuRegs->D0MD&0x7)==0x7) )
         {
            if (ScuRegs->dma0.TransferNumber != 0) {
              ScuDmaProc(ScuRegs, 0x7FFFFFFF);
            }
            ScuRegs->dma0.mode = 0;
            ScuRegs->dma0.ReadAddress = ScuRegs->D0R;
            ScuRegs->dma0.WriteAddress = ScuRegs->D0W;
            ScuRegs->dma0.TransferNumber = ScuRegs->D0C;
            ScuRegs->dma0.AddValue = ScuRegs->D0AD;
            ScuRegs->dma0.ModeAddressUpdate = ScuRegs->D0MD;
            ScuSetAddValue(&ScuRegs->dma0);
            ScuDmaProc(ScuRegs, 128);
         }
         ScuRegs->D0EN = val;
         break;
      case 0x14:
         ScuRegs->D0MD = val;
         break;
      case 0x20:
         ScuRegs->D1R = val;
         break;
      case 0x24:
         ScuRegs->D1W = val;
         break;
      case 0x28:
         ScuRegs->D1C = val;
         break;
      case 0x2C:
         ScuRegs->D1AD = val;
         break;
      case 0x30:
      if ((val & 0x1) && ((ScuRegs->D1MD&0x07) == 0x7))
         {
            if (ScuRegs->dma1.TransferNumber != 0) {
              ScuDmaProc(ScuRegs, 0x7FFFFFFF);
            }

            ScuRegs->dma1.mode = 1;
            ScuRegs->dma1.ReadAddress = ScuRegs->D1R;
            ScuRegs->dma1.WriteAddress = ScuRegs->D1W;
            ScuRegs->dma1.TransferNumber = ScuRegs->D1C;
            ScuRegs->dma1.AddValue = ScuRegs->D1AD;
            ScuRegs->dma1.ModeAddressUpdate = ScuRegs->D1MD;
            ScuSetAddValue(&ScuRegs->dma1);
            ScuDmaProc(ScuRegs,128);
         }
         ScuRegs->D1EN = val;
         break;
      case 0x34:
         ScuRegs->D1MD = val;
         break;
      case 0x40:
         ScuRegs->D2R = val;
         break;
      case 0x44:
         ScuRegs->D2W = val;
         break;
      case 0x48:
         ScuRegs->D2C = val;
         break;
      case 0x4C:
         ScuRegs->D2AD = val;
         break;
      case 0x50:
      if ((val & 0x1) && ((ScuRegs->D2MD & 0x7) == 0x7))
         {

            if (ScuRegs->dma2.TransferNumber != 0) {
              ScuDmaProc(ScuRegs, 0x7FFFFFFF);
            }

            ScuRegs->dma2.mode = 2;
            ScuRegs->dma2.ReadAddress = ScuRegs->D2R;
            ScuRegs->dma2.WriteAddress = ScuRegs->D2W;
            ScuRegs->dma2.TransferNumber = ScuRegs->D2C;
            ScuRegs->dma2.AddValue = ScuRegs->D2AD;
            ScuRegs->dma2.ModeAddressUpdate = ScuRegs->D2MD;
            ScuSetAddValue(&ScuRegs->dma2);
            ScuDmaProc(ScuRegs, 128);
         }
         ScuRegs->D2EN = val;
         break;
      case 0x54:
         ScuRegs->D2MD = val;
         break;
      case 0x60:
         ScuRegs->DSTP = val;
         break;
      case 0x7C:
        ScuRegs->DSTA = val;
        break;
      case 0x80: // DSP Program Control Port
         LOG("scu: wrote %08X to DSP Program Control Port", val);
         ScuDsp->ProgControlPort.all = (ScuDsp->ProgControlPort.all & 0x00FC0000) | (val & 0x060380FF);

         if (ScuDsp->ProgControlPort.part.LE) {
            // set pc
            ScuDsp->PC = (u8)ScuDsp->ProgControlPort.part.P;
         }

         // Execution is rquested
         if (val & 0x10000) {
           // clear internal values
           ScuDsp->jmpaddr = 0xffffffff;
         }
         break;
      case 0x84: // DSP Program Ram Data Port
         //LOG("scu: wrote %08X to DSP Program ram offset %02X", val, ScuDsp->PC);
         ScuDsp->ProgramRam[ScuDsp->PC] = val;
         ScuDsp->PC++;
         ScuDsp->ProgControlPort.part.P = ScuDsp->PC;
         break;
      case 0x88: // DSP Data Ram Address Port
         //LOG("scu: wrote %08X to DSP Data Ram ", val);
         //ScuDsp->DataRamPage = (val >> 6) & 3;
         ScuDsp->DataRamReadAddress = val;
         break;
      case 0x8C: // DSP Data Ram Data Port
         //LOG("scu: wrote %08X to DSP Data Ram Data Port Page %d offset %02X", val, ScuDsp->DataRamPage, ScuDsp->DataRamReadAddress);
         if (!ScuDsp->ProgControlPort.part.EX) {
            ScuDsp->MD[ (ScuDsp->DataRamReadAddress >> 6) & 0x03][ ScuDsp->DataRamReadAddress & 0x3F ] = val;
            ScuDsp->DataRamReadAddress++;
         }
         break;
      case 0x90:
         ScuRegs->T0C = val;
         break;
      case 0x94:
         ScuRegs->T1S = val;
         ScuRegs->timer1_set = 1;
         ScuRegs->timer1_preset = val;
         break;
      case 0x98:
         ScuRegs->T1MD = val;
         break;
      case 0xA0:
         ScuRegs->IMS = val;
         LOG("IMS = %X PC=%X frame=%d:%d", val, CurrentSH2->regs.PC, yabsys.frame_count,yabsys.LineCount);
         ScuTestInterruptMask();
         break;
      case 0xA4: {
        u32 after = ScuRegs->IST & val;
        LOG("IST = from %X to %X PC=%X frame=%d:%d", ScuRegs->IST, after, CurrentSH2->regs.PC, yabsys.frame_count, yabsys.LineCount);
        ScuRemoveInterruptByCPU(ScuRegs->IST, after);
        ScuRegs->IST = after;
        ScuTestInterruptMask();
      }
         break;
      case 0xA8:
         ScuRegs->AIACK = val;
         ScuTestInterruptMask();
         break;
      case 0xB0:
         ScuRegs->ASR0 = val;
         break;
      case 0xB4:
         ScuRegs->ASR1 = val;
         break;
      case 0xB8:
         ScuRegs->AREF = val;
         break;
      case 0xC4:
         ScuRegs->RSEL = val;
         break;
      default:
         LOG("Unhandled SCU Register long write %08X\n", addr);
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void ScuRemoveInterruptByCPU(u32 pre, u32 after) {
  for (int i = 0; i < 16; i++) {
    if (((pre >> i) & 0x01) && (((after >> i) & 0x01) == 0)) {
      u32 ii, i2;
      int hit = -1;
      for (ii = 0; ii < ScuRegs->NumberOfInterrupts; ii++) {
        if (ScuRegs->interrupts[i].statusbit == (1<<i)) {
          hit = ii;
          ScuRegs->IST &= ~ScuRegs->interrupts[i].statusbit;
          LOG("%s(%0X) is removed at frame %d:%d", ScuGetVectorString(ScuRegs->interrupts[i].vector), ScuRegs->interrupts[i].vector, yabsys.frame_count, yabsys.LineCount);
          break;
        }
      }
      if (hit != -1) {
        i2 = 0;
        for (ii = 0; ii < ScuRegs->NumberOfInterrupts; ii++) {
          if (ii != hit) {
            memcpy(&ScuRegs->interrupts[i2], &ScuRegs->interrupts[ii], sizeof(scuinterrupt_struct));
            i2++;
          }
        }
        ScuRegs->NumberOfInterrupts--;
      }
    }
  }
}

void ScuTestInterruptMask()
{
   unsigned int i, i2;

   // Handle SCU interrupts
   for (i = 0; i < ScuRegs->NumberOfInterrupts; i++)
   {
     u32 mask = ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].mask;

     // A-BUS?
     if (mask & 0x8000){
       if (ScuRegs->AIACK){
         ScuRegs->AIACK = 0;
         if (!(ScuRegs->IMS & 0x8000)) {

           const u8 vector = ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].vector;
           SH2SendInterrupt(MSH2, vector, ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].level);

           if (yabsys.IsSSH2Running) {
             if (vector == 0x42)
               SH2SendInterrupt(SSH2, 0x41, 1);
             if (vector == 0x40)
               SH2SendInterrupt(SSH2, 0x43, 2);
           }

           ScuRegs->IST &= ~ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].statusbit;

           // Shorten list
           for (i2 = ScuRegs->NumberOfInterrupts - 1 - i; i2 < (ScuRegs->NumberOfInterrupts - 1); i2++)
             memcpy(&ScuRegs->interrupts[i2], &ScuRegs->interrupts[i2 + 1], sizeof(scuinterrupt_struct));

           ScuRegs->NumberOfInterrupts--;
           ScuRegs->AIACK = 0;
         }
       }
     }else if (!(ScuRegs->IMS & mask)) {

       // removed manually
       if ( (ScuRegs->IST & ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].statusbit) == 0) {

         //LOG("removed");

       }
       else {
         const u8 vector = ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].vector;
         LOG("%s(%0X) IST=%08X delay at frame %d:%d", ScuGetVectorString(vector), vector, ScuRegs->IST, yabsys.frame_count, yabsys.LineCount);

         SH2SendInterrupt(MSH2, vector, ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].level);

         if (yabsys.IsSSH2Running) {
           if (vector == 0x42)
             SH2SendInterrupt(SSH2, 0x41, 1);
           if (vector == 0x40)
             SH2SendInterrupt(SSH2, 0x43, 2);
         }

         ScuRegs->IST &= ~ScuRegs->interrupts[ScuRegs->NumberOfInterrupts - 1 - i].statusbit;

         // Shorten list
         for (i2 = ScuRegs->NumberOfInterrupts - 1 - i; i2 < (ScuRegs->NumberOfInterrupts - 1); i2++)
           memcpy(&ScuRegs->interrupts[i2], &ScuRegs->interrupts[i2 + 1], sizeof(scuinterrupt_struct));

         ScuRegs->NumberOfInterrupts--;
         break;
       }
      }
   }
}

//////////////////////////////////////////////////////////////////////////////
static void ScuQueueInterrupt(u8 vector, u8 level, u16 mask, u32 statusbit)
{
   u32 i, i2;
   scuinterrupt_struct tmp;

   // Make sure interrupt doesn't already exist
   for (i = 0; i < ScuRegs->NumberOfInterrupts; i++)
   {
      if (ScuRegs->interrupts[i].vector == vector)
         return;
   }

   ScuRegs->interrupts[ScuRegs->NumberOfInterrupts].vector = vector;
   ScuRegs->interrupts[ScuRegs->NumberOfInterrupts].level = level;
   ScuRegs->interrupts[ScuRegs->NumberOfInterrupts].mask = mask;
   ScuRegs->interrupts[ScuRegs->NumberOfInterrupts].statusbit = statusbit;
   ScuRegs->NumberOfInterrupts++;

   // Sort interrupts
   for (i = 0; i < (ScuRegs->NumberOfInterrupts-1); i++)
   {
      for (i2 = i+1; i2 < ScuRegs->NumberOfInterrupts; i2++)
      {
         if (ScuRegs->interrupts[i].level > ScuRegs->interrupts[i2].level)
         {
            memcpy(&tmp, &ScuRegs->interrupts[i], sizeof(scuinterrupt_struct));
            memcpy(&ScuRegs->interrupts[i], &ScuRegs->interrupts[i2], sizeof(scuinterrupt_struct));
            memcpy(&ScuRegs->interrupts[i2], &tmp, sizeof(scuinterrupt_struct));
         }
      }
   }
}

void ScuRemoveInterrupt(u8 vector, u8 level, u32 statusbit){
   ScuRegs->IST &= ~statusbit;

   int i2 = 0;
   int ii = 0;
   int hit = -1;

   // find pending interrupt
   for (ii = 0; ii < ScuRegs->NumberOfInterrupts; ii++) {
      if( ScuRegs->interrupts[ii].vector == vector ) {
         hit = ii;
         break;
      }
   }

   // remove pending interrupt
   if( hit != -1 ){
      for (ii = 0; ii < ScuRegs->NumberOfInterrupts; ii++) {
         if (ii != hit) {
            memcpy(&ScuRegs->interrupts[i2], &ScuRegs->interrupts[ii], sizeof(scuinterrupt_struct));
            i2++;
         }
      }
      ScuRegs->NumberOfInterrupts--;
   }

}

//////////////////////////////////////////////////////////////////////////////

static INLINE void SendInterrupt(u8 vector, u8 level, u16 mask, u32 statusbit) {

  // A-BUS?
  if ((mask & 0x8000) ){
    if (ScuRegs->AIACK){
      ScuRegs->AIACK = 0;
      if (!(ScuRegs->IMS & 0x8000)){
        SH2SendInterrupt(MSH2, vector, level);
      }
    }
  }else if (!(ScuRegs->IMS & mask)){

    ScuRegs->IST |= statusbit;
    //if (vector != 0x41) LOG("INT %d", vector);
    LOG("%s(%x) IMS=%08X at frame %d:%d", ScuGetVectorString(vector), vector, ScuRegs->IMS, yabsys.frame_count, yabsys.LineCount);
    SH2SendInterrupt(MSH2, vector, level);
    if (yabsys.IsSSH2Running) {
      if (vector == 0x42)
        SH2SendInterrupt(SSH2, 0x41, 1);
      if (vector == 0x40)
        SH2SendInterrupt(SSH2, 0x43, 2);
    }
  }
  else
   {
      //LOG("%s(%x) is Queued IMS=%08X %d:%d", ScuGetVectorString(vector), vector, ScuRegs->IMS, yabsys.frame_count, yabsys.LineCount);
      ScuQueueInterrupt(vector, level, mask, statusbit);
      ScuRegs->IST |= statusbit;
   }
}

// 3.2 DMA control register
static INLINE void ScuChekIntrruptDMA(int id){

  if ((ScuRegs->D0EN & 0x100) && (ScuRegs->D0MD & 0x07) == id){
    if (ScuRegs->dma0.TransferNumber > 0) {
      ScuDmaProc(ScuRegs, 0x7FFFFFFF);
    }
    ScuRegs->dma0.mode = 0;
    ScuRegs->dma0.ReadAddress = ScuRegs->D0R;
    ScuRegs->dma0.WriteAddress = ScuRegs->D0W;
    ScuRegs->dma0.TransferNumber = ScuRegs->D0C;
    ScuRegs->dma0.AddValue = ScuRegs->D0AD;
    ScuRegs->dma0.ModeAddressUpdate = ScuRegs->D0MD;
    ScuSetAddValue(&ScuRegs->dma0);
    ScuDmaProc(ScuRegs, 128);
    ScuRegs->D0EN = 0;
  }
  if ((ScuRegs->D1EN & 0x100) && (ScuRegs->D1MD & 0x07) == id){
    if (ScuRegs->dma1.TransferNumber > 0) {
      ScuDmaProc(ScuRegs, 0x7FFFFFFF);
    }
    ScuRegs->dma1.mode = 1;
    ScuRegs->dma1.ReadAddress = ScuRegs->D1R;
    ScuRegs->dma1.WriteAddress = ScuRegs->D1W;
    ScuRegs->dma1.TransferNumber = ScuRegs->D1C;
    ScuRegs->dma1.AddValue = ScuRegs->D1AD;
    ScuRegs->dma1.ModeAddressUpdate = ScuRegs->D1MD;
    ScuSetAddValue(&ScuRegs->dma1);
    ScuDmaProc(ScuRegs, 128);
    ScuRegs->D1EN = 0;
  }
  if ((ScuRegs->D2EN & 0x100) && (ScuRegs->D2MD & 0x07) == id){
    if (ScuRegs->dma2.TransferNumber > 0) {
      ScuDmaProc(ScuRegs, 0x7FFFFFFF);
    }
    ScuRegs->dma2.mode = 2;
    ScuRegs->dma2.ReadAddress = ScuRegs->D2R;
    ScuRegs->dma2.WriteAddress = ScuRegs->D2W;
    ScuRegs->dma2.TransferNumber = ScuRegs->D2C;
    ScuRegs->dma2.AddValue = ScuRegs->D2AD;
    ScuRegs->dma2.ModeAddressUpdate = ScuRegs->D2MD;
    ScuSetAddValue(&ScuRegs->dma2);
    ScuDmaProc(ScuRegs, 128);
    ScuRegs->D2EN = 0;
  }
}

void ScuRemoveInterrupt(u8 vector, u8 level, u32 statusbit);
void ScuRemoveVBlankOut();
void ScuRemoveHBlankIN();
void ScuRemoveVBlankIN();
void ScuRemoveTimer0();
void ScuRemoveTimer1();

//////////////////////////////////////////////////////////////////////////////

void ScuSendVBlankIN(void) {
   //ScuRemoveVBlankOut();
   //ScuRemoveHBlankIN();
   ScuRemoveTimer0();
   SendInterrupt(0x40, 0xF, 0x0001, 0x0001);
   ScuChekIntrruptDMA(0);

}


//if (vector == 0x42)
//SH2SendInterrupt(SSH2, 0x41, 1);
//if (vector == 0x40)
//SH2SendInterrupt(SSH2, 0x43, 2);

void ScuRemoveVBlankIN() {
  ScuRemoveInterrupt(0x40, 0x0F, 0x0001);
  //SH2RemoveInterrupt(MSH2, 0x40, 0x0F);
  //SH2RemoveInterrupt(SSH2, 0x43, 0x0F);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendVBlankOUT(void) {
   SendInterrupt(0x41, 0xE, 0x0002, 0x0002);

   // Pending VBlankin interrput on CPU must be cleared here
   ScuRemoveVBlankIN();

   ScuRemoveTimer0();
   ScuRemoveTimer1();
   ScuRegs->timer0 = 0;

   if (ScuRegs->T1MD & 0x1)
   {
     if (ScuRegs->timer0 == ScuRegs->T0C) {
       ScuRegs->timer0_set = 1;
       ScuSendTimer0();
     }
     else {
       ScuRegs->timer0_set = 0;
       ScuRemoveTimer0();
     }
   }

   ScuChekIntrruptDMA(1);
}

void ScuRemoveVBlankOut() {
  ScuRemoveInterrupt(0x41, 0x0E, 0x02 );
  //SH2RemoveInterrupt(MSH2, 0x41, 0x0E);
}

//////////////////////////////////////////////////////////////////////////////

void ScuRemoveHBlankIN() {
  ScuRemoveInterrupt(0x42, 0x0D, 0x0004);
  //SH2RemoveInterrupt(MSH2, 0x42, 0x0D);
  //SH2RemoveInterrupt(SSH2, 0x41, 0x0D);
}


void ScuSendHBlankIN(void) {
   //if(yabsys.LineCount == 0) ScuRemoveVBlankOut();
   SendInterrupt(0x42, 0xD, 0x0004, 0x0004);
   ScuRegs->timer0++;
   if (ScuRegs->T1MD & 0x1)
   {
      // if timer0 equals timer 0 compare register, do an interrupt
     if (ScuRegs->timer0 == ScuRegs->T0C) {
        ScuSendTimer0();
        ScuRegs->timer0_set = 1;
     }
     else {
       ScuRegs->timer0_set = 0;
       ScuRemoveTimer0();
     }

     if (ScuRegs->timer1_set == 1) {
        ScuRegs->timer1_set = 0;
        ScuRegs->timer1_counter = ScuRegs->timer1_preset;
        ScuRemoveTimer1();
      }
   }
   ScuChekIntrruptDMA(2);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendTimer0(void) {
   SendInterrupt(0x43, 0xC, 0x0008, 0x00000008);
   ScuChekIntrruptDMA(3);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendTimer1(void) {
   SendInterrupt(0x44, 0xB, 0x0010, 0x00000010);
   ScuChekIntrruptDMA(4);
}

void ScuRemoveTimer0(void) {
  ScuRemoveInterrupt(0x43, 0x0C, 0x00000008);
  //SH2RemoveInterrupt(MSH2, 0x43, 0x0C);
}


void ScuRemoveTimer1(void) {
  ScuRemoveInterrupt(0x44, 0x0B, 0x00000010);
  //SH2RemoveInterrupt(MSH2, 0x44, 0xB);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendDSPEnd(void) {
   SendInterrupt(0x45, 0xA, 0x0020, 0x00000020);
   //ScuRemoveInterrupt(0x45, 0xA, 0x0020, 0x00000020);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendSoundRequest(void) {
   SendInterrupt(0x46, 0x9, 0x0040, 0x00000040);
   ScuChekIntrruptDMA(5);
   //ScuRemoveInterrupt(0x46, 0x9, 0x0040, 0x00000040);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendSystemManager(void) {
   SendInterrupt(0x47, 0x8, 0x0080, 0x00000080);
   //ScuRemoveInterrupt(0x47, 0x8, 0x0080, 0x00000080);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendPadInterrupt(void) {
   SendInterrupt(0x48, 0x8, 0x0100, 0x00000100);
   //ScuRemoveInterrupt(0x48, 0x8, 0x0100, 0x00000100);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendDMAEnd(u32 mode)
{
	u32 mask = 0x800 >> mode;
	SendInterrupt(0x4B - mode, 0x6 - !mode, mask, mask);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendDMAIllegal(void) {
   SendInterrupt(0x4C, 0x3, 0x1000, 0x00001000);
   //ScuRemoveInterrupt(0x4C, 0x3, 0x1000, 0x00001000);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendDrawEnd(void) {
   SendInterrupt(0x4D, 0x2, 0x2000, 0x00002000);
   ScuChekIntrruptDMA(6);
   //ScuRemoveInterrupt(0x4D, 0x2, 0x2000, 0x00002000);
}

//////////////////////////////////////////////////////////////////////////////

void ScuSendExternalInterrupt00(void) {
   SendInterrupt(0x50, 0x7, 0x8000, 0x00010000);
}
