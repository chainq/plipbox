/*
** $VER: server.c 1.15 (01 Apr 1998)
**
** magplip.device - Parallel Line Internet Protocol
**
** Original code written by Oliver Wagner and Michael Balzer.
**
** This version has been completely reworked by Marius Gröger, introducing
** slight protocol changes. The new source is a lot better organized and
** maintainable.
**
** Additional changes and code cleanup by Jan Kratochvil and Martin Mares.
** The new source is significantly faster and yet better maintainable.
**
** (C) Copyright 1993-1994 Oliver Wagner & Michael Balzer
** (C) Copyright 1995 Jan Kratochvil & Martin Mares
** (C) Copyright 1995-1996 Marius Gröger
**     All Rights Reserved
**
** $HISTORY:
**
** 01 Apr 1998 : 001.015 :  integrated modifications for linPLIP from Stephane
** 19 Mar 1998 : 001.014 :  fixed S2_ONLINE bug, which returned an
**                          error by a call to go online if it was
**                          already online. Report from Holger Kruse.
**                          Fixed by Stefan Ruppert.
** 29 Mar 1996 : 001.014 :  changed copyright note
** 24 Feb 1996 : 001.013 :  added PRTRSEL data direction signal
** 30 Dec 1995 : 001.012 :  + dynamic allocation of only one frame buffer
**                          PLIP_MAXMTU now 128k
**                          + a bad MTU setting in ENV: will be
**                          forced to PLIP_MAXMTU instead of PLIP_DEFMTU
**                          + server task acknowledge now after calling
**                          readargs to avoid inconsistencies
** 03 Sep 1995 : 001.011 :  hardware addressing nicer
** 30 Aug 1995 : 001.010 :  + support for timer-timed timeout :-)
**                          + minor declaration related changes
** 20 Aug 1995 : 001.009 :  support for ASM xfer routines
**                          removed obsolete CIA macros (mag/jk/mm)
** 29 Jul 1995 : 001.008 :  support for arbitration delay
**                          symmetrical handling
** 26 Apr 1995 : 001.007 :  _very_ nasty bug would miss packets and get
**                          the driver totally irritated
** 25 Apr 1995 : 001.006 :  now compiles with ANSI and STRICT
**                          fixed bug with resource allocation
** 08 Mar 1995 : 001.005 :  write req. are now handled by device.c
** 06 Mar 1995 : 001.004 :  collision delay added
** 06 Mar 1995 : 001.003 :  hardware transmission errors are no longer retried
**                          because this is any upper layers job
** 04 Mar 1995 : 001.002 :  event tracking *much* more conform to SANA-2
** 18 Feb 1995 : 001.001 :  startup now a bit nicer
**                          using BASEPTR
** 12 Feb 1995 : 001.000 :  reworked original
*/

#define DEBUG 15

/*F*/ /* includes */
#ifndef CLIB_EXEC_PROTOS_H
#include <clib/exec_protos.h>
#include <pragmas/exec_sysbase_pragmas.h>
#endif
#ifndef CLIB_DOS_PROTOS_H
#include <clib/dos_protos.h>
#include <pragmas/dos_pragmas.h>
#endif

#ifndef EXEC_MEMORY_H
#include <exec/memory.h>
#endif

#ifndef DEVICES_SANA2_H
#include <devices/sana2.h>
#endif

#ifndef _STRING_H
#include <string.h>
#endif

#ifndef __MAGPLIP_H
#include "magplip.h"
#endif
#ifndef __DEBUG_H
#include "debug.h"
#endif
#ifndef __COMPILER_H
#include "compiler.h"
#endif
#ifndef __HW_H
#include "hw.h"
#endif
/*E*/

/*F*/ /* defines, types and enums */

   /*
   ** return codes for arbitratedwrite()
   */
typedef enum { AW_OK, AW_ABORTED, AW_BUFFER_ERROR, AW_ERROR } AW_RESULT;

   /* return val, cut to min or max if exceeding range */
#define BOUNDS(val, min, max) ((val) <= (max) ? ((val) >= (min) ? (val) :\
                                                         (min)) : (max))

/*E*/
/*F*/ /* imports */
   /* external functions */
GLOBAL VOID dotracktype(BASEPTR, ULONG type, ULONG ps, ULONG pr, ULONG bs, ULONG br, ULONG pd);
GLOBAL VOID DevTermIO(BASEPTR, struct IOSana2Req *ios2);
/*E*/
/*F*/ /* exports */
PUBLIC VOID SAVEDS ServerTask(void);
/*E*/
/*F*/ /* private */
PRIVATE struct PLIPBase *startup(void);
PRIVATE REGARGS VOID DoEvent(BASEPTR, long event);
PRIVATE VOID readargs(BASEPTR);
PRIVATE BOOL init(BASEPTR);
PRIVATE REGARGS BOOL goonline(BASEPTR);
PRIVATE REGARGS VOID gooffline(BASEPTR);
PRIVATE REGARGS AW_RESULT arbitratedwrite(BASEPTR, struct IOSana2Req *ios2);
PRIVATE REGARGS VOID dowritereqs(BASEPTR);
PRIVATE REGARGS VOID doreadreqs(BASEPTR);
PRIVATE REGARGS VOID dos2reqs(BASEPTR);
/*E*/

   /*
   ** functions to go online/offline
   */
/*F*/ PRIVATE REGARGS VOID rejectpackets(BASEPTR)
{
   struct IOSana2Req *ios2;

   ObtainSemaphore(&pb->pb_WriteListSem);
   while(ios2 = (struct IOSana2Req *)RemHead((struct List*)&pb->pb_WriteList))
   {
      ios2->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
      ios2->ios2_WireError = S2WERR_UNIT_OFFLINE;
      DevTermIO(pb,ios2);
   }
   ReleaseSemaphore(&pb->pb_WriteListSem);

   ObtainSemaphore(&pb->pb_ReadListSem);
   while(ios2 = (struct IOSana2Req *)RemHead((struct List*)&pb->pb_ReadList))
   {
      ios2->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
      ios2->ios2_WireError = S2WERR_UNIT_OFFLINE;
      DevTermIO(pb,ios2);
   }
   ReleaseSemaphore(&pb->pb_ReadListSem);

   ObtainSemaphore(&pb->pb_ReadOrphanListSem);
   while(ios2 = (struct IOSana2Req *)RemHead((struct List*)&pb->pb_ReadOrphanList))
   {
      ios2->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
      ios2->ios2_WireError = S2WERR_UNIT_OFFLINE;
      DevTermIO(pb,ios2);
   }
   ReleaseSemaphore(&pb->pb_ReadOrphanListSem);
}
/*E*/

/*F*/ PRIVATE REGARGS BOOL goonline(BASEPTR)
{
   BOOL rc = TRUE;

   d(("trying to go online\n"));

   if (pb->pb_Flags & (PLIPF_OFFLINE | PLIPF_NOTCONFIGURED))
   {
      if (!hw_attach(pb))
      {
         d(("error going online\n"));
         rc = FALSE;
      }
      else
      {
         /* TODO: missing TimeBase */
         /*GetSysTime(&pb->pb_DevStats.LastStart);*/
         pb->pb_Flags &= ~(PLIPF_OFFLINE | PLIPF_NOTCONFIGURED);
         DoEvent(pb, S2EVENT_ONLINE);
         d(("i'm now online!\n"));
      }
   }

   return rc;
}
/*E*/
/*F*/ PRIVATE REGARGS VOID gooffline(BASEPTR)
{
   if (!(pb->pb_Flags & (PLIPF_OFFLINE | PLIPF_NOTCONFIGURED)))
   {
      hw_detach(pb);

      pb->pb_Flags |= PLIPF_OFFLINE;

      DoEvent(pb, S2EVENT_OFFLINE);
   }
   d(("ok!\n"));
}
/*E*/

   /*
   ** SANA-2 Event management
   */
/*F*/ PRIVATE REGARGS VOID DoEvent(BASEPTR, long event)
{
   struct IOSana2Req *ior, *ior2;

   d(("event is %lx\n",event));

   ObtainSemaphore(&pb->pb_EventListSem );
   
   for(ior = (struct IOSana2Req *) pb->pb_EventList.lh_Head;
       ior2 = (struct IOSana2Req *) ior->ios2_Req.io_Message.mn_Node.ln_Succ;
       ior = ior2 )
   {
      if (ior->ios2_WireError & event)
      {
         Remove((struct Node*)ior);
         DevTermIO(pb, ior);
      }
   }
   
   ReleaseSemaphore(&pb->pb_EventListSem );
}
/*E*/

   /*
   ** writing packets
   */
/*F*/ PRIVATE REGARGS AW_RESULT arbitratedwrite(BASEPTR, struct IOSana2Req *ios2)
{
   BOOL can_send;
   AW_RESULT rc;
   struct HWFrame *frame = pb->pb_Frame;

   can_send = hw_begin_send(pb);
   if (can_send)
   {
      struct BufferManagement *bm;

      UBYTE *frame_ptr;
      
      d(("having line for: type %08lx, size %ld\n",ios2->ios2_PacketType,
                                                   ios2->ios2_DataLength));

      /* copy raw frame: simply overwrite ethernet frame part of plip packet */
      if(ios2->ios2_Req.io_Flags & SANA2IOF_RAW) {
         frame->hwf_Size = ios2->ios2_DataLength + HW_EXTRA_HDR_SIZE;
         frame_ptr = &frame->hwf_DstAddr[0];
      } else {
         frame->hwf_Size = ios2->ios2_DataLength + HW_EXTRA_HDR_SIZE + HW_ETH_HDR_SIZE;
         frame->hwf_Type = (USHORT)ios2->ios2_PacketType;
         memcpy(frame->hwf_SrcAddr, pb->pb_CfgAddr, HW_ADDRFIELDSIZE);
         memcpy(frame->hwf_DstAddr, ios2->ios2_DstAddr, HW_ADDRFIELDSIZE);
         frame_ptr = (UBYTE *)(frame + 1);
      }

      bm = (struct BufferManagement *)ios2->ios2_BufferManagement;

      if (!(*bm->bm_CopyFromBuffer)(frame_ptr,
                                  ios2->ios2_Data, ios2->ios2_DataLength))
      {
         rc = AW_BUFFER_ERROR;
         hw_abort_send(pb);
      }
      else
      {
         d8(("+hw_send\n"));
         rc = hw_send_frame(pb, frame) ? AW_OK : AW_ERROR;
         d8(("-hw_send\n"));
#if DEBUG&8
         if(rc==AW_ERROR) d8(("Error sending packet (size=%ld)\n", (LONG)pb->pb_Frame->hwf_Size));
#endif
      }
   }
   else
      rc = AW_ABORTED;

   return rc;
}
/*E*/
/*F*/ PRIVATE REGARGS VOID dowritereqs(BASEPTR)
{
   struct IOSana2Req *currentwrite, *nextwrite;
   AW_RESULT code;
   struct HWBase *hwb = &pb->pb_HWBase;

   ObtainSemaphore(&pb->pb_WriteListSem);

   for(currentwrite = (struct IOSana2Req *)pb->pb_WriteList.lh_Head;
       nextwrite = (struct IOSana2Req *) currentwrite->ios2_Req.io_Message.mn_Node.ln_Succ;
       currentwrite = nextwrite )
   {
      if (hw_recv_pending(pb))
      {
         d(("incoming data!"));
         break;
      }

      code = arbitratedwrite(pb, currentwrite);

      if (code == AW_ABORTED)                         /* arbitration failed */
      {
         pb->pb_Flags |= PLIPF_COLLISION;
         d(("couldn't get the line, trying again later\n"));
         pb->pb_SpecialStats[S2SS_COLLISIONS].Count++;
         d(("pb->pb_SpecialStats[S2SS_COLLISIONS].Count = %ld\n",pb->pb_SpecialStats[S2SS_COLLISIONS].Count));
         if ((currentwrite->ios2_Req.io_Error++) > pb->pb_Retries)
         {
            pb->pb_SpecialStats[S2SS_TXERRORS].Count++;
            d(("pb->pb_SpecialStats[S2SS_TXERRORS].Count = %ld\n",pb->pb_SpecialStats[S2SS_TXERRORS].Count));
            currentwrite->ios2_Req.io_Error = S2ERR_TX_FAILURE;
            currentwrite->ios2_WireError = S2WERR_TOO_MANY_RETIRES;
            Remove((struct Node*)currentwrite);
            DevTermIO(pb, currentwrite);
         }
         break;
      }
      else if (code == AW_BUFFER_ERROR)  /* BufferManagement callback error */
      {
         d(("buffer error\n"));
         DoEvent(pb, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
         pb->pb_SpecialStats[S2SS_TXERRORS].Count++;
         d(("pb->pb_SpecialStats[S2SS_TXERRORS].Count = %ld\n",pb->pb_SpecialStats[S2SS_TXERRORS].Count));
         currentwrite->ios2_Req.io_Error = S2ERR_SOFTWARE;
         currentwrite->ios2_WireError = S2WERR_BUFF_ERROR;
         Remove((struct Node*)currentwrite);
         DevTermIO(pb, currentwrite);
      }
      else if (code == AW_ERROR)
      {
         /*
         ** this is a real line error, upper levels (e.g. Internet TCP) have
         ** to care for reliability!
         */
         d(("error while transmitting packet\n"));
         DoEvent(pb, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_HARDWARE);
         pb->pb_SpecialStats[S2SS_TXERRORS].Count++;
         d(("pb->pb_SpecialStats[S2SS_TXERRORS].Count = %ld\n",pb->pb_SpecialStats[S2SS_TXERRORS].Count));
         currentwrite->ios2_Req.io_Error = S2ERR_TX_FAILURE;
         currentwrite->ios2_WireError = S2WERR_GENERIC_ERROR;
         Remove((struct Node*)currentwrite);
         DevTermIO(pb, currentwrite);
      }
      else /*if (code == AW_OK)*/                             /* well done! */
      {
         d(("packet transmitted successfully\n"));
         pb->pb_DevStats.PacketsSent++;
         dotracktype(pb, (ULONG) pb->pb_Frame->hwf_Type, 1, 0, currentwrite->ios2_DataLength, 0, 0);
         currentwrite->ios2_Req.io_Error = S2ERR_NO_ERROR;
         currentwrite->ios2_WireError = S2WERR_GENERIC_ERROR;
         Remove((struct Node*)currentwrite);
         DevTermIO(pb, currentwrite);
      }
   }

   ReleaseSemaphore(&pb->pb_WriteListSem);
}
/*E*/

PRIVATE REGARGS BOOL deliverreadreq(struct IOSana2Req *req, struct HWFrame *frame)
{
   int i;
   BOOL broadcast; 
   LONG datasize;
   BYTE *frame_ptr;
   struct BufferManagement *bm;
   BOOL ok;
   
   /* deliver a raw frame: copy data right into ethernet header */
   if(req->ios2_Req.io_Flags & SANA2IOF_RAW) {
      frame_ptr = &frame->hwf_DstAddr[0];
      datasize = frame->hwf_Size - HW_EXTRA_HDR_SIZE;
      req->ios2_Req.io_Flags = SANA2IOF_RAW;
   }
   else {
      frame_ptr = (UBYTE *)(frame + 1);
      datasize = frame->hwf_Size - (HW_EXTRA_HDR_SIZE + HW_ETH_HDR_SIZE);
      req->ios2_Req.io_Flags = 0;
   }

   req->ios2_DataLength = datasize;
   
   /* copy packet buffer */
   bm = (struct BufferManagement *)req->ios2_BufferManagement;
   if (!(*bm->bm_CopyToBuffer)(req->ios2_Data, frame_ptr, datasize))
   {
      d(("CopyToBuffer: error\n"));
      req->ios2_Req.io_Error = S2ERR_SOFTWARE;
      req->ios2_WireError = S2WERR_BUFF_ERROR;
      ok = FALSE;
   }
   else
   {
      req->ios2_Req.io_Error = req->ios2_WireError = 0;
      ok = TRUE;
   }
   
   /* now extract addresses from ethernet header */
   memcpy(req->ios2_SrcAddr, frame->hwf_SrcAddr, HW_ADDRFIELDSIZE);
   memcpy(req->ios2_DstAddr, frame->hwf_DstAddr, HW_ADDRFIELDSIZE);
   
   /* need to set broadcast flag? */
   broadcast = TRUE;
   for(i=0;i<HW_ADDRFIELDSIZE;i++) {
      if(frame->hwf_DstAddr[i] != 0xff) {
         broadcast = FALSE;
         break;
      }
   }
   if(broadcast) {
      req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
   }
   
   return ok;
}

   /*
   ** reading packets
   */
/*F*/ PRIVATE REGARGS VOID doreadreqs(BASEPTR)
{
   LONG datasize;
   struct IOSana2Req *got;
   ULONG pkttyp;
   BOOL rv;
   struct HWFrame *frame = pb->pb_Frame;

   d8(("+hw_recv\n"));
   rv = hw_recv_frame(pb, frame);
   d8(("-hw_recv\n"));
   if (rv)
   {
      pb->pb_DevStats.PacketsReceived++;

      datasize = frame->hwf_Size - (HW_EXTRA_HDR_SIZE + HW_ETH_HDR_SIZE);

      dotracktype(pb, pkttyp = frame->hwf_Type, 0, 1, 0, datasize, 0);

      d(("packet %08lx, size %ld received\n",pkttyp,datasize));

      ObtainSemaphore(&pb->pb_ReadListSem);

         /* traverse the list of read-requests */
      for(got = (struct IOSana2Req *)pb->pb_ReadList.lh_Head;
          got->ios2_Req.io_Message.mn_Node.ln_Succ;
          got = (struct IOSana2Req *)got->ios2_Req.io_Message.mn_Node.ln_Succ )
      {
            /* check if this one requests for the new packet we got */
         if (got->ios2_PacketType == pkttyp )
         {
            BOOL ok;
            
            Remove((struct Node*)got);

            /* deliver packet */
            ok = deliverreadreq(got, frame);
            if(!ok) {
               DoEvent(pb, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
            }

            d(("packet received, satisfying S2Request\n"));
            DevTermIO(pb, got);
            got = NULL;
            break;
         }
      }

      ReleaseSemaphore(&pb->pb_ReadListSem);
   }
   else
   {
      d8(("Error receiving (%ld. len=%ld)\n", rv, frame->hwf_Size));
      /* something went wrong during receipt */
      DoEvent(pb, S2EVENT_HARDWARE | S2EVENT_ERROR | S2EVENT_RX);
      got = NULL;
      pb->pb_DevStats.BadData++;
   }

      /* If no one wanted this packet explicitely, there is one chance
      ** left: somebody waiting for orphaned packets. If this fails, too,
      ** we will drop it.
      */
   if (got)
   {
      d(("unknown packet\n"));

      pb->pb_DevStats.UnknownTypesReceived++;
      
      ObtainSemaphore(&pb->pb_ReadOrphanListSem);
      got = (struct IOSana2Req *)RemHead((struct List*)&pb->pb_ReadOrphanList);
      ReleaseSemaphore(&pb->pb_ReadOrphanListSem);

      if (got)
      {
         BOOL ok = deliverreadreq(got, frame);
         if(!ok) {
            DoEvent(pb, S2EVENT_ERROR | S2EVENT_BUFF | S2EVENT_SOFTWARE);
         }

         d(("orphan read\n"));

         DevTermIO(pb, got);
      }
      else
      {
         dotracktype(pb, pkttyp, 0, 0, 0, 0, 1);
         d(("packet thrown away...\n"));
      }
   }
}
/*E*/

   /*
   ** 2nd level device command dispatcher (~SANA2IOF_QUICK)
   */
/*F*/ PRIVATE REGARGS VOID dos2reqs(BASEPTR)
{
   struct IOSana2Req *ios2;

   /*
   ** Every pending IO message will be GetMsg()'ed and processed. At the
   ** end of the loop it will be DevTermIO()'ed back to the sender,
   ** _but_only_if_ it is non-NULL. In such cases the message has been
   ** put in a separate queue to be DevTermIO()'ed later (i.e. CMD_WRITEs
   ** and similar stuff).
   ** You find the same mimique in the 1st level dispatcher (device.c)
   */
   while(ios2 = (struct IOSana2Req *)GetMsg(pb->pb_ServerPort))
   {
      if (hw_recv_pending(pb))
      {
         d(("incoming data!"));
         break;
      }

      d(("sana2req %ld from serverport\n", ios2->ios2_Req.io_Command));

      switch (ios2->ios2_Req.io_Command)
      {
         case S2_ONLINE:
            if (!goonline(pb))
            {
               ios2->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
               ios2->ios2_WireError = S2WERR_GENERIC_ERROR;
            }
         break;

         case S2_OFFLINE:
            gooffline(pb);
            rejectpackets(pb); /* reject all pending requests */
         break;

         case S2_CONFIGINTERFACE:
            if (pb->pb_Flags & PLIPF_NOTCONFIGURED)
            {
               /* copy address from src addr */
               memcpy(pb->pb_CfgAddr, ios2->ios2_SrcAddr, HW_ADDRFIELDSIZE);
               if (!goonline(pb))
               {
                  ios2->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                  ios2->ios2_WireError = S2WERR_GENERIC_ERROR;
               }
            }
            else
            {
               ios2->ios2_Req.io_Error = S2ERR_BAD_STATE;
               ios2->ios2_WireError = S2WERR_IS_CONFIGURED;
            }
         break;
      }

      if (ios2) DevTermIO(pb,ios2);
   }
}
/*E*/

   /*
   ** startup,initialisation and termination functions
   */
/*F*/ PRIVATE struct PLIPBase *startup(void)
{
   struct ServerStartup *ss;
   struct Process *we;
   struct PLIPBase *base;
   LOCALSYSBASE;

   we = (struct Process*)FindTask(NULL);

   d(("waiting for startup msg...\n"));
   WaitPort(&we->pr_MsgPort);
   ss = (struct ServerStartup *)GetMsg(&we->pr_MsgPort);
   base = ss->ss_PLIPBase;
   base->pb_Startup = ss;
   d(("go startup msg at %lx, PLIPBase is %lx\n", ss, ss->ss_PLIPBase));

   /* we will keep the startup message, to inform mother if we
   ** really could come up or if we failed to obtain some
   ** resource.
   */
   return base;
}
/*E*/
/*F*/ PRIVATE VOID readargs(BASEPTR)
{
   struct RDArgs *rda;
   struct PLIPConfig args = { 0 };
   BPTR plipvar, oldinput;

   d(("entered\n"));

   if (plipvar = Open(CONFIGFILE, MODE_OLDFILE))
   {
      oldinput = SelectInput(plipvar);
      
      rda = ReadArgs(TEMPLATE , (LONG *)&args, NULL);
      
      if(rda)
      {
         if (args.timeout)
            pb->pb_Timeout =
                  BOUNDS(*args.timeout, PLIP_MINTIMEOUT, PLIP_MAXTIMEOUT);

         if (args.priority)
            SetTaskPri((struct Task*)pb->pb_Server,
                  BOUNDS(*args.priority, PLIP_MINPRIORITY, PLIP_MAXPRIORITY));

         if (args.mtu)
            pb->pb_MTU = BOUNDS(*args.mtu, PLIP_MINMTU, PLIP_MAXMTU);

         if (args.bps)
            pb->pb_ReportBPS = BOUNDS(*args.bps, PLIP_MINBPS, PLIP_MAXBPS);

         if (args.retries)
            pb->pb_Retries =
                     BOUNDS(*args.retries, PLIP_MINRETRIES, PLIP_MAXRETRIES);

         if (args.sendcrc)
            pb->pb_Flags |= PLIPF_SENDCRC;
          else
            pb->pb_Flags &= ~PLIPF_SENDCRC;

         if (args.collisiondelay)
            pb->pb_CollisionDelay =
               BOUNDS(*args.collisiondelay, PLIP_MINCOLLISIONDELAY,
                                            PLIP_MAXCOLLISIONDELAY);
         else
            pb->pb_CollisionDelay = PLIP_DEFDELAY + (pb->pb_Unit ?
                                                  PLIP_DELAYDIFF : 0);

         if (args.arbitrationdelay)
            pb->pb_ArbitrationDelay =
               BOUNDS(*args.collisiondelay, PLIP_MINARBITRATIONDELAY,
                                            PLIP_MAXARBITRATIONDELAY);
         else
            pb->pb_ArbitrationDelay = PLIP_DEFARBITRATIONDELAY;

         if (args.nospecialstats)
            pb->pb_ExtFlags |= PLIPEF_NOSPECIALSTATS;

         FreeArgs(rda);
      }

      Close(SelectInput(oldinput));
   }

   d(("timeout %ld, pri %ld, mtu %ld, bps %ld, retries %ld, flags %08lx, delay %ld\n",
      pb->pb_Timeout, (LONG)pb->pb_Server->pr_Task.tc_Node.ln_Pri, pb->pb_MTU, pb->pb_ReportBPS, pb->pb_Retries,
      pb->pb_Flags, pb->pb_CollisionDelay));

   d(("left\n"));

}
/*E*/
/*F*/ PRIVATE BOOL init(BASEPTR)
{
   BOOL rc = FALSE;

   readargs(pb);
      
   if ((pb->pb_ServerPort = CreateMsgPort()))
   {  
      /* init hardware */
      if(hw_init(pb)) {         
         d(("allocating 0x%lx/%ld bytes frame buffer\n",
                  sizeof(struct HWFrame)+pb->pb_MTU,
                  sizeof(struct HWFrame)+pb->pb_MTU));
         if ((pb->pb_Frame = AllocVec((ULONG)sizeof(struct HWFrame) +
                                       pb->pb_MTU, MEMF_CLEAR|MEMF_ANY)))
         {
            rc = TRUE;
         }
         else
         {
            d(("couldn't allocate frame buffer\n"));
         }
      }
      else
      {
         d(("hw init failed\n"));
      }
   }
   else
   {
      d(("no server port\n"));
   }

   d(("left %ld\n",rc));

   return rc;
}
/*E*/
/*F*/ PRIVATE VOID cleanup(BASEPTR)
{
   struct BufferManagement *bm;

   gooffline(pb);

   while(bm = (struct BufferManagement *)RemHead((struct List *)&pb->pb_BufferManagement))
      FreeVec(bm);

   if (pb->pb_Frame) FreeVec(pb->pb_Frame);

   hw_cleanup(pb);

   if (pb->pb_ServerPort) DeleteMsgPort(pb->pb_ServerPort);

   if (pb->pb_Flags & PLIPF_REPLYSS)
   {
      Forbid();
      ReplyMsg((struct Message*)pb->pb_Startup);
   }
}
/*E*/

   /*
   ** entry point, mainloop
   */
/*F*/ PUBLIC VOID SAVEDS ServerTask(void)
{
   BASEPTR;

   d(("server running\n"));

   if (pb = startup())
   {      
         /* if we fail to allocate all resources, this flag reminds cleanup()
         ** to ReplyMsg() the startup message
         */
      pb->pb_Flags |= PLIPF_REPLYSS;

      if (init(pb))
      {
         ULONG recv=0, portsigmask, hwsigmask, wmask;
         BOOL running;

         /* Ok, we are fine and will tell this mother personally :-) */
         pb->pb_Startup->ss_Error = 0;
         /* don't forget this, or we will have to keep a warm place */
         /* in our coffin for the system */
         pb->pb_Flags &= ~PLIPF_REPLYSS;
         ReplyMsg((struct Message*)pb->pb_Startup);

         portsigmask  = 1 << pb->pb_ServerPort->mp_SigBit;
         hwsigmask = hw_get_sigmask(pb);
      
         wmask = SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C | portsigmask | hwsigmask;

         /* main loop of server task */
         d2(("--- server main loop: %08lx ---\n", wmask));
         for(running=TRUE;running;)
         {
            d(("** wmask is 0x%08lx\n", wmask));

            /* if no recv is pending then wait for incoming signals */
            if (!hw_recv_pending(pb)) {
               d2(("**> wait\n"));
               recv = Wait(wmask);
            }

            /* accept pending receive and start reading */
            d2(("* rx pending\n"));
            if (hw_recv_pending(pb))
            {
               d2(("* rx ack\n"));
               hw_recv_ack(pb);
               
               d2(("*+ do_read\n"));
               doreadreqs(pb);
               d2(("*- do_read\n"));
            }

            /* handle other hw signals, e.g. timeouts */
            if(recv & hwsigmask)
            {
               d2(("*+ hw sigmask\n"));
               hw_handle_sigmask(pb, recv & hwsigmask);
               d2(("*- hw sigmask\n"));
            }
            
            /* can we send packets? */
            d2(("* can send\n"));
            if(hw_can_send(pb)) {
               d2(("*+ do_write\n"));
               dowritereqs(pb);
               d2(("*- do_write\n"));
            }
            
            /* if pending send now */
            d2(("* hw_recv_pending\n"));
            if (hw_recv_pending(pb))
            {
               d2(("* hw_recv_ack\n"));
               hw_recv_ack(pb);
               
               d2(("*+ do_read2\n"));
               doreadreqs(pb);
               d2(("*- do_read2\n"));
            }
            
            /*
            ** Possible a collision has occurred, which is indicated by a
            ** special flag in PLIPBase.
            **
            ** Using timer.device we periodically will be waked up. This
            ** allows us to delay write packets in cases when we cannot get
            ** the line immediately.
            **
            ** If client and server are very close together, regarding the point
            ** of performance, the same delay time could even force multiple
            ** collisions (at least theoretical, I made no practical tests).
            ** Probably a CSMA/CD-like random-timed delay would be ideal.
            */
            if (pb->pb_Flags & PLIPF_COLLISION)
            {
               pb->pb_Flags &= ~PLIPF_COLLISION;
               d2(("*+ coll\n"));
               hw_handle_collision(pb);
               d2(("*- coll\n"));
            }
            
            /* handle SANA-II send requests */
            if (recv & portsigmask)
            {
               d(("SANA-II request(s)\n"));
               dos2reqs(pb);
            }

            /* stop server task */
            if (recv & SIGBREAKF_CTRL_C)
            {
               d(("received break signal\n"));
               running = FALSE;
            }
         }
      }
      else
         d(("init() failed\n"));

      d2(("--- server exit main loop ---\n"));
      cleanup(pb);

            /* Exec will enable it's scheduler after we're dead. */
      Forbid();
            /* signal mother we're done */
      if (pb->pb_ServerStoppedSigMask)
         Signal(pb->pb_Task, pb->pb_ServerStoppedSigMask);
      pb->pb_Flags |= PLIPF_SERVERSTOPPED;
   }
   else
      d(("no startup packet\n"));
}
/*E*/

