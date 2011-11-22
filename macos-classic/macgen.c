/*Copyright �1999-2003 Mars SaxmanPalette code copyright � 1999 Dave WinzlerAltiVec code copyright � 2001 Scott MarcyAll Rights ReservedThis program is free software; you can redistribute it and/ormodify it under the terms of the GNU General Public Licenseas published by the Free Software Foundation; either version 2of the License, or (at your option) any later version.This program is distributed in the hope that it will be useful,but WITHOUT ANY WARRANTY; without even the implied warranty ofMERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See theGNU General Public License for more details.You should have received a copy of the GNU General Public Licensealong with this program; if not, write to the Free SoftwareFoundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.The Mac Starfish image creator.This is the module that takes the numbers Starfish spits out and putsthem into a pixel buffer suitable for display on a MacOS machine.Since the Mac is not multithreaded, this code has to behave under acooperative multitasking system. Instead of creating a buffer, dumpingpixels to it, and returning, we do the work in stages.When the main body of the application detects some idle time, it callsour work routine MacGeneratorCompute. One of the parameters is a timevalue, measured in ticks. We work until that amount of time has expired,then return. This way we don't lock up the machine for more than animperceptible length of time, which would otherwise annoy the user.*/#include <stdlib.h>#include <string.h>#include <stdio.h>#include <time.h>#include <Quickdraw.h>#include <Palettes.h>#include <Multiprocessing.h>#include <Events.h>#include <Resources.h>#include <Folders.h>#include <Sound.h>#include <ImageCompression.h>#include <Debugging.h>#include <Gestalt.h>#include "macgen.h"#include "starfish-engine.h"#include "setdesktop.h"#include "preferences.h"#include "starfish-altivec.h"#include "mp_macgen.h"//We don't create patterns any smaller than this value.#define SMALL_MIN 64//Medium patterns should be at least this big.#define MED_MIN 96//Large patterns must be at least this big.#define LARGE_MIN 192//There is no upper limit.//how many pixels do we process at a time?#define COL_CHUNK 64#if BUILD_MPBoolean			gDoingMP = false;#endif#if BUILD_ALTIVECextern bool	gUseAltivec;#endifstatic UInt32	gRandomSeed;static void			CalcStarfishPalette(int palette, StarfishPalette* it);static Boolean		CompressToJPEG(PixMapHandle pixH, Handle *data);static void			CreateRandomSize(int sizecode, SInt16* width, SInt16* height);static GWorldPtr	CreateSizedGWorld(int sizecode);static int			GenerateTextureGWorldLine(GWorldPtr dest, StarfishRef texture, int v, int max, int* h);static int			GenerateTextureGWorldLine_AV(GWorldPtr dest, StarfishRef texture, int v, int max);static PicHandle	CopyGWorldToPICT(GWorldPtr input);static void			SavePictToDesktop(Handle it);static OSErr		WritePictFile(Handle it);#if TEST_ALTIVEC_GENERATORSstatic void RecordAltivecAccuracy(MacGenRef it);#endifBoolean CanUseAltivec(void){#if BUILD_ALTIVEC	OSErr	err;	long	processorAttributes;	Boolean	hasAltiVec = false;	err = Gestalt(gestaltPowerPCProcessorFeatures, &processorAttributes);	if (err == noErr)		hasAltiVec = (processorAttributes & (1 << gestaltPowerPCHasVectorInstructions));	return hasAltiVec;#else	return false;#endif} // CanUseAltivecstatic void CalcStarfishPalette(int paletteID, StarfishPalette* it)	{	PaletteHandle	hpltt;	short			nEntries, i;	// copy the specified palette resource to prefs	// dave winzler, 7/31/99	// modified by Mars to use GetIndResource and to support	// palette randomisation	if (paletteID == paletteFullSpectrum) {		it->colourcount = 0;	}	else {		if (paletteID == paletteRandom)	{			paletteID = (rand() * Count1Resources('pltt') / RAND_MAX) + 1;			hpltt = (PaletteHandle)Get1IndResource('pltt', paletteID);		}		else	{			hpltt = (PaletteHandle)Get1Resource('pltt', paletteID);		}		// copy the specified palette resource into prefs		if (hpltt) {			nEntries = (*hpltt)->pmEntries;			if (nEntries > MAX_PALETTE_ENTRIES)				nEntries = MAX_PALETTE_ENTRIES;			it->colourcount = nEntries;			for (i=0; i<nEntries; ++i) {				it->colour[i].red = (**hpltt).pmInfo[i].ciRGB.red >> 8;				it->colour[i].green = (**hpltt).pmInfo[i].ciRGB.green >> 8;				it->colour[i].blue = (**hpltt).pmInfo[i].ciRGB.blue >> 8;			}		}		else {			it->colourcount = 0;		}	}	}static GWorldPtr CreateSizedGWorld(int sizecode)	{	/*	Make a GWorld of the size the user specified.	If that doesn't work, fall down to the next smallest size,	and so on until we get one.	Or we fail altogether.	*/	Rect tempframe;	GWorldPtr out = NULL;	OSErr err = noErr;	tempframe.top = tempframe.left = 0;	CreateRandomSize(sizecode, &tempframe.right, &tempframe.bottom);	err = NewGWorld(&out, 32, &tempframe, NULL, NULL, keepLocal | pixelsLocked | useTempMem);	if(err) out = NULL;	if(out)		{		GWorldPtr oldworld;		GDHandle olddevice;		GetGWorld(&oldworld, &olddevice);		SetGWorld(out, olddevice);		EraseRect(&tempframe);		SetGWorld(oldworld, olddevice);		}	return out;	}MacGenRef MakeMacGenerator(int sizecode)	{	/*	Create all the components necessary to generate a Mac	starfish pattern.	Create the starfish generator itself.	Create a GWorld to hold the data.	The size code is a suggestion for how big the pattern should be.	If we can't make a pattern that big due to memory constraints,	it's OK to make one smaller. The point is, to make *some* pattern.	Hmmm. I think. I'll have to see how that goes.	*/	StarfishPalette colours;	StarfishPalette* colourptr = NULL;	MacGenRef out = (MacGenRef)malloc(sizeof(MacGen));	if(out)	{#if BUILD_ALTIVEC		gUseAltivec = CanUseAltivec();#endif#if BUILD_MP		gDoingMP = CanUseMP();#endif#if TEST_ALTIVEC_GENERATORS		gNumPixelsGenerated = gNumAltivecPixelsOOB = 0;#endif		//Scramble the random seed. We don't want to make the same patterns more than once.#if RAND_SEED		gRandomSeed = RAND_SEED;			// Use the same seed so we generate the same thing each time. This allows us to see how much performance we've gained#else		gRandomSeed = time(NULL);#endif		srand(gRandomSeed);		out->generator = NULL;		out->dest = NULL;		// Try to initialize the MP stuff#if BUILD_MP		if (gDoingMP)			gDoingMP = InitMP();#endif		//Calculate a set of colours for this pattern.		if(gPrefs.palette != paletteFullSpectrum)			{			CalcStarfishPalette(gPrefs.palette, &colours);			colourptr = &colours;			}		//If the user picked "random", make the pattern whatever size will fit.		if(sizecode == sizeCodeRandom) sizecode = ((rand() * SIZE_CODE_RANGE) / RAND_MAX) + 1;		while(sizecode >= sizeCodeSmall && !out->generator)			{			//Create a GWorld to store the destination image.			out->dest = CreateSizedGWorld(sizecode);			//Next create a pattern to match the size of this GWorld.			if(out->dest)				{				Rect portRect;				GetPortBounds(out->dest, &portRect);				out->curline = portRect.top;				out->maxlines = portRect.bottom;				out->curcol = portRect.left;				out->maxcol = portRect.right;				out->generator = MakeStarfish(portRect.right - portRect.left, out->maxlines, colourptr, sizecode != sizeCodeFullScreen );				if(out->generator)					{					/*					Just for safety, allocate a whole gob of extra RAM roughly the same					size as we expect the PicHandle and script handle to be when this is					all finished. We'll throw this RAM away immediately, but if we fail					to allocate it, that's a good sign that we'd run into trouble later					on if we tried to proceed with a pattern this big.					*/					Ptr scratchiness;					scratchiness = NewPtr(portRect.right * portRect.bottom * 4 + 0x7FFF);					if(scratchiness)						{						//We succeeded! Throw away this scratch memory and get on with life.						DisposePtr(scratchiness);						}					else						{						//We failed. Throw away the GWorld and generator and try again.						DisposeGWorld(out->dest);						out->dest = NULL;						DumpStarfish(out->generator);						out->generator = NULL;						sizecode--;						}					}				else					{					//Couldn't create the generator. Dump the GWorld and try again smaller.					DisposeGWorld(out->dest);					out->dest = NULL;					sizecode--;					}				}			else				{				//Oops. Couldn't create the GWorld. Reduce the size code to try for less RAM.				sizecode--;				}			}#if BUILD_MP		if(!out->generator || !out->dest || (gDoingMP && StartMPTasks(out) != noErr))	// Start the MP tasks#else		if(!out->generator || !out->dest)#endif			{			//We failed. Throw away the MacGen record.			if(out->generator) DumpStarfish(out->generator);			if(out->dest) DisposeGWorld(out->dest);			free(out);			out = NULL;			}#if BUILD_MP			else if (!gDoingMP) {		// Don't try to profile the MP code				StartProfiling();				SuspendProfiling();			} // if/else#endif		}	return out;	}int MacGeneratorProgress(MacGenRef it)	{	//We process things one line at a time. The current progress	//also happens to be the current line completed.	int out = 0;	if(it)		{#if BUILD_MP		if (gDoingMP)			out = MP_GeneratorProgress();		else#endif			out = it->curline;		}	return out;	}int MacGeneratorMax(MacGenRef it)	{	/*	One line equals one processing unit.	So the maximum number of lines must equal the maximum	number of processing units.	*/	int out = 0;	if(it)		{		out = it->maxlines;		}	return out;	}int MacGeneratorDone(MacGenRef it)	{	/*	We are done if the current line is equal to or greater than	the maximum number of lines.	*/	int out = false;	if(it)		{#if BUILD_MP		if (gDoingMP)			out = MP_GeneratorDone();		else#endif			out = it->curline >= it->maxlines;		}	return out;	}void MacGeneratorCompute(int maxticks, MacGenRef it)	{	/*	Do computation work on this generator.	Essentially, we generate pattern lines until our time runs out.	The caller tells us how much time we have.	*/	SInt32 endticks = TickCount() + maxticks;#if BUILD_MP	if (gDoingMP)	{	// If we're using MP, there's nothing for us to do here!		MPYield();		return;	} // if#endif	while(endticks > TickCount() && it->curline < it->maxlines)		{		ResumeProfiling();#if BUILD_ALTIVEC		if (gUseAltivec)			it->curline = GenerateTextureGWorldLine_AV(it->dest, it->generator, it->curline, it->maxlines - it->curline);		else#endif			it->curline = GenerateTextureGWorldLine(it->dest, it->generator, it->curline, it->maxlines - it->curline, &it->curcol);		SuspendProfiling();		}	}void DumpMacGenerator(MacGenRef it)	{	if(it)		{#if BUILD_MP		if (gDoingMP)			StopMPTasks(it);		// Doesn't return until all MP tasks are gone		else#endif			StopProfiling();		if(it->dest) DisposeGWorld(it->dest);#if TEST_ALTIVEC_GENERATORS		RecordAltivecAccuracy(it);#endif		DumpStarfish(it->generator);		free(it);		}	}GWorldPtr PeekMacGeneratorWorld(MacGenRef it)	{	/*	Return a reference to the MacGenerator's GWorld.	This lets outsiders see what our image looks like	while we're working on it. We maintain ownership	of the GWorld; hanging onto this reference is not advised.	*/	GWorldPtr out = NULL;	if(it)		{		out = it->dest;		}	return out;	}/* *	Name:			CompressToJPEG * *	Parameters:		pixH : handle to the PixMap image to compress *					data : returns handle with JPEG data * *	Returns:		True if successful, false if not * *	Description:	Compresses the PixMap given using JPEG compression. *					A new handle containing the JPEG picture data is *					allocated by this routine and returned in 'data'. *					The caller must dispose of this handle. * */static Boolean CompressToJPEG(PixMapHandle pixH, Handle *data){OSErr					err;long					dsize;ImageDescriptionHandle	descH;Rect					r;Handle					h;Ptr						p;	// Initialize	*data = nil;	r = (*pixH)->bounds;	// Determine the maximum output size	err = GetMaxCompressionSize(pixH, &r, 0, codecNormalQuality, 'jpeg', anyCodec, &dsize);	if (err != noErr)		return false;	// Need some handles	descH = (ImageDescriptionHandle) NewHandle(4);	if (descH == nil)		return false;	h = NewHandle(dsize);	if (h == nil) {		DisposeHandle((Handle) descH);		return false;	} // if	// Lock and load!	MoveHHi(h);	HLock(h);	//p = StripAddress(*h);	p = *h;	err = CompressImage(pixH, &r, codecNormalQuality, 'jpeg', descH, p);	if (err == noErr) {		HUnlock(h); p = nil;		SetHandleSize(h, (*descH)->dataSize);		*data = h;	} else		DisposeHandle(h);		// Dump the output buffer on error	DisposeHandle((Handle) descH);	return (err == noErr);} // CompressToJPEGvoid WriteGeneratedImageToDesktop(MacGenRef it)	{	if (gPrefs.fileType == typeCodeJPEG) {		PixMapHandle		destpix = GetGWorldPixMap(it->dest);		Handle					data;		if (CompressToJPEG(destpix, &data)) {			SavePictToDesktop(data);			DisposeHandle(data);		} // if	} else {		/*		We have a GWorld.		Get the contents of the GWorld as a PICT.		If we were successful, write the PICT to our file in		the system folder, and call up the desktop control panel to install it.		*/		PicHandle scratch;		if(it && it->dest)		{			scratch = CopyGWorldToPICT(it->dest);			if(scratch)			{				SavePictToDesktop((Handle) scratch);				KillPicture(scratch);			}		}	} // if/else} // WriteGeneratedImageToDesktopstatic void CreateRandomSize(int sizecode, SInt16* h, SInt16* v)	{	/*	Based on the suggestion of the given size-code, make up a random	size for the output pattern.	We range from MIN_SIZE to the width/height of the main monitor.	*/	GDHandle screen;	int maxWidth, maxHeight, combine;	screen = GetMainDevice();	if(screen)		{		maxWidth = (*screen)->gdRect.right - (*screen)->gdRect.left;		maxHeight = (*screen)->gdRect.bottom - (*screen)->gdRect.top;		switch(sizecode)			{			case sizeCodeFullScreen:				//This one's easy. Just use the monitor dimensions.				*h = maxWidth;				*v = maxHeight;				break;			case sizeCodeLarge:				//For large patterns, we average the width and height.				//The output values range between 1/4 and 1/2 that value.				//The value must be at least 256, regardless of monitor size.				combine = (maxWidth + maxHeight) / 8;				if(combine < LARGE_MIN) combine = LARGE_MIN;				*h = ((rand() * combine) / RAND_MAX) + combine;				*v = ((rand() * combine) / RAND_MAX) + combine;				break;			case sizeCodeMedium:				//Medium patterns are similar to large patterns.				//The output values range between 1/8 and 1/4 screen average.				combine = (maxWidth + maxHeight) / 16;				if(combine < MED_MIN) combine = MED_MIN;				*h = ((rand() * combine) / RAND_MAX) + combine;				*v = ((rand() * combine) / RAND_MAX) + combine;				break;			case sizeCodeSmall:				//Small patterns range from SMALL_MIN to 1/16 of the monitor.				combine = (maxWidth + maxHeight) / 32;				*h = ((rand() * combine) / RAND_MAX) + SMALL_MIN;				*v = ((rand() * combine) / RAND_MAX) + SMALL_MIN;				break;			default:				//If we don't recognize it, make it small.				*h = SMALL_MIN;				*v = SMALL_MIN;				break;			}		}	}static int GenerateTextureGWorldLine(GWorldPtr dest, StarfishRef texture, int v, int max, int* starth)	{	/*	Generate one row of data for this GWorld.	We fill in all the appropriate pixels for just that row.	We have to clean up everything we mess up along the way,	because we don't know what will happen in between calls.	If we have two processors, we calculate two lines at a time,	setting the second processor to work on the next line while	we continue working on the first.	*/	pixel srlColor;	PixMapHandle destpix;	Ptr pixBaseAddr;	Ptr thisLinePix;	unsigned char* thisPixel;	SInt16 rowbytes;	int out = 0;#if BUILD_MP	if (gDoingMP) {		DebugStr("\pShould never call GenerateTextureGWorldLine() when using MP!");		return max;	// This will stop the loop that calls us	} // if#endif	Rect portRect;	GetPortBounds(dest, &portRect);		if(dest && texture && v >= portRect.top && v < portRect.bottom && max > 0)		{		int h, hmax, hmin, endloop;		hmin = portRect.left;		hmax = portRect.right;		endloop = hmax;		v -= portRect.top;		if(starth)			{			if(*starth >= hmax) *starth = hmin;			if(hmax - *starth > COL_CHUNK) endloop = *starth + COL_CHUNK;			}		destpix = GetGWorldPixMap(dest);		LockPixels(destpix);				pixBaseAddr = (*destpix)->baseAddr;		rowbytes = (*destpix)->rowBytes & 0x7FFF;		thisLinePix = (Ptr)((UInt32)pixBaseAddr + (v * rowbytes));		for(h = starth ? *starth : hmin; h < endloop; h++)			{			//Calculate the pixel value at the current location.			GetStarfishPixel(h - hmin, v - portRect.top, texture, &srlColor);			//Now put this point at the appropriate spot in the grafport.			thisPixel = (unsigned char*)((UInt32)thisLinePix + (h * 4));			thisPixel[0] = srlColor.alpha;			thisPixel[1] = srlColor.red;			thisPixel[2] = srlColor.green;			thisPixel[3] = srlColor.blue;			}		//Return the next line that should be computed.		//Our next call will begin with this line.		if(endloop == hmax) out = v + 1;			else out = v;		if(starth) *starth = h;		UnlockPixels(GetGWorldPixMap(dest));		}	return out;	}#if BUILD_ALTIVECstatic int GenerateTextureGWorldLine_AV(GWorldPtr dest, StarfishRef texture, int v, int max){	/*	Generate one row of data for this GWorld.	We fill in all the appropriate pixels for just that row.	We have to clean up everything we mess up along the way,	because we don't know what will happen in between calls.	If we have two processors, we calculate two lines at a time,	setting the second processor to work on the next line while	we continue working on the first.	*/	PixMapHandle	destpix;	Ptr				pixBaseAddr;	UInt32*			thisLinePix;	SInt16			rowbytes;	int				out = 0;#if BUILD_MP	if (gDoingMP) {		DebugStr("\pShould never call GenerateTextureGWorldLine_AV() when using MP!");		return max;	// This will stop the loop that calls us	} // if#endif	Rect portRect;	GetPortBounds(dest, &portRect);		if(dest && texture && v >= portRect.top && v < portRect.bottom && max > 0)	{		int h, hmax, hmin, endloop, extra;		hmin = portRect.left;		hmax = portRect.right;		endloop = hmax;		v -= portRect.top;		destpix = GetGWorldPixMap(dest);		LockPixels(destpix);				h = hmin;		pixBaseAddr = (*destpix)->baseAddr;		rowbytes = (*destpix)->rowBytes & 0x7FFF;		thisLinePix = (UInt32*)((UInt32)pixBaseAddr + (v * rowbytes) + (hmin * 4));		extra = endloop % PIXELS_PER_CALL;	// Number of extra pixels		endloop -= extra;					// make it an even multiple		while (h < endloop)		{			//Calculate the pixel value at the current location.			GetStarfishPixel_AV(h - hmin, v - portRect.top, texture, (vector unsigned char*) thisLinePix);			thisLinePix += PIXELS_PER_CALL;			h += PIXELS_PER_CALL;		} // while		if (extra != 0) {			vector unsigned char		temp[PIXELS_PER_CALL / 4];	// 4 pixels per vector			GetStarfishPixel_AV(h - hmin, v - portRect.top, texture, temp);			BlockMoveData(temp, thisLinePix, extra * sizeof(UInt32));			h += extra;		} // if		//Return the next line that should be computed.		//Our next call will begin with this line.		out = v + 1;		UnlockPixels(GetGWorldPixMap(dest));	}	return out;} // GenerateTextureGWorldLine_AV#endif	// BUILD_ALTIVECstatic PicHandle CopyGWorldToPICT(GWorldPtr input)	{	/*	Copy the GWorld we were just given into a picture handle.	*/	PicHandle out = NULL;	GWorldPtr oldworld;	GDHandle olddevice;	//Save the current GWorld before we go any further.	GetGWorld(&oldworld, &olddevice);	if(input)		{		//Set our output to the new GWorld.		SetGWorld(input, NULL);		//Open up a picture handle - kind of like pushing		//"record" on a tape recorder.		Rect portRect;		GetPortBounds(input, &portRect);		out = OpenPicture(&portRect);		if(out)			{				//(BitMap *)(*(*gGWorld).portPixMap)			#if CARBON			CopyBits(GetPortBitMapForCopyBits(input), GetPortBitMapForCopyBits(input), &portRect, &portRect, ditherCopy, NULL);			#else			CopyBits((BitMap*)(*(*input).portPixMap), (BitMap*)(*(*input).portPixMap), &portRect, &portRect, ditherCopy, NULL);			#endif			ClosePicture();			}		//Go back to whatever GWorld was active when we started this job.		SetGWorld(oldworld, olddevice);		}	return out;	}static void SavePictToDesktop(Handle it)	{	/*	Save the given picture into a file in the System folder.	We use the same name every time, so that we don't clutter up	the folder with excess picture files.	*/	WritePictFile(it);#if !SUPPRESS_OUTPUT	SetDesktopToSavedFile();#endif	}static OSErr WritePictFile(Handle it)	{	/*	If our destination file already exists, open it.	Otherwise, create it from scratch.	It will be a PICT file in the System folder with whatever name we were given.	*/	OSErr	err = noErr;	SInt16	fileref;	long	bytesout;	FSSpec	destfile;	// Setup the filename for this file	destfile.name[0] = sprintf((char*) destfile.name+1, "starfish.pict");	/*	Try to save the picture into the "Desktop Pictures" folder first.	If that folder does not exist, save it directly into the System folder.	*/	err = FindFolder(kOnSystemDisk, kDesktopPicturesFolderType, kDontCreateFolder, &destfile.vRefNum, &destfile.parID);	if(err) err = FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &destfile.vRefNum, &destfile.parID);	if(!err)		{		//Attempt to create the file.		err = FSpCreate(&destfile, 'ttxt', gPrefs.fileType == typeCodeJPEG ? 'JPEG' : 'PICT', 0);		if (err == dupFNErr) {			FInfo	info;			// Set the correct file type/creator			err = FSpGetFInfo(&destfile, &info);			if (err == noErr) {				info.fdType    = (gPrefs.fileType == typeCodeJPEG ? 'JPEG' : 'PICT');				info.fdCreator = 'ttxt';				err = FSpSetFInfo(&destfile, &info);			} // if		} // if		//Now open it.		err = FSpOpenDF(&destfile, fsRdWrPerm, &fileref);		if(!err)			{			//Zero out the file so no junk gets put in at the end			SetEOF(fileref, 0);			if (gPrefs.fileType != typeCodeJPEG) {				Ptr		temp = NewPtrClear(512);				//Write 512 bytes of junk as a header - this is part of the PICT file definition.				bytesout = 512;				// Stuff the random seed in the first four bytes				if (temp != nil)					*((UInt32*) temp) = gRandomSeed;				FSWrite(fileref, &bytesout, temp ? temp : (Ptr) &destfile);				if (temp != nil)					DisposePtr(temp);			} // if			//Now lock the picture handle and write its contents as well.			HLock(it);			bytesout = GetHandleSize(it);			FSWrite(fileref, &bytesout, *it);			HUnlock(it);			//That's it. Close the file; we're done.			FSClose(fileref);			fileref = 0;			}		}	return err;	}#if TEST_ALTIVEC_GENERATORSstatic void RecordAltivecAccuracy(MacGenRef it){	char			buff[256];	float			oobPercent = ((float) gNumAltivecPixelsOOB / (float) gNumPixelsGenerated) * 100.0;	ConstStringPtr	name = "\pStarfish Altivec Test Log";	short			refnum;	long			count;	char*			more = NULL;	count = sprintf(buff, "Pattern generated from seed %9d had %9d out-of-bounds pixels out of %9d total pixels (%12.8f%%)\n",			gRandomSeed, gNumAltivecPixelsOOB, gNumPixelsGenerated, oobPercent);	// If we're more than 1% out-of-bounds, get more details so we can try to reproduce it	if (oobPercent >= 1.0)		more = GetStarfishStatus_AV(it->generator);	HCreate(-1, fsRtDirID, name, 'R*ch', 'TEXT');	if (HOpenDF(-1, fsRtDirID, name, fsRdWrPerm, &refnum) == noErr) {		SetFPos(refnum, fsFromLEOF, 0);	// Move to end of file		FSWrite(refnum, &count, buff);		if (more != NULL) {			count = strlen(more);			FSWrite(refnum, &count, more);		} // if		FSClose(refnum);	} else		SysBeep(1);	if (more != NULL)		free(more);} // RecordAltivecAccuracy#endif/////////////////////// Profiling Stuff ///////////////////////#pragma mark -#if (__profile__)void StartProfiling(void){	OSErr		err;	err = ProfilerInit(collectDetailed,bestTimeBase, 1500, 500);	if (err == noErr) {		ProfilerSetStatus(false);		SysBeep(1);						// alert us that we are running a profiled version	} else		DebugStr("\pProfilerInit returned error.");} // StartProfilingstatic void GetVolumeName(short vRefNum, StringPtr volNamePtr){	HVolumeParam	vparam;	vparam.ioNamePtr = volNamePtr;	vparam.ioVRefNum = vRefNum;	vparam.ioVolIndex = 0;	PBHGetVInfoSync((HParmBlkPtr) &vparam);} // GetVolumeNamevoid StopProfiling(void){	Str27	vol;	Str63	path;	ProfilerSetStatus(false);	GetVolumeName(-1, vol);	// Get the name of the startup disk//	path[0] = sprintf((char*) path+1, "%#s:Starfish.profile", vol);	path[0] = sprintf((char*) path+1, "Spare:Starfish.profile", vol);	ProfilerDump(path);	ProfilerTerm();} // StopProfiling#endif