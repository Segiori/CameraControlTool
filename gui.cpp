//#include <gtk/gtk.h>
#include <gtkmm.h>
#include <iostream>
#include <gtkmm/application.h>

#include "stdio.h"
#include "cordef.h"
#include "GenApi/GenApi.h"		//!< GenApi lib definitions.
#include "gevapi.h"				//!< GEV lib definitions.
#include "SapX11Util.h"
#include "X_Display_utils.h"
#include "FileUtil.h"
#include <sched.h>

//using namespace std;
//using namespace GenICam;
//using namespace GenApi;

#define MAX_NETIF					8
#define MAX_CAMERAS_PER_NETIF	32
#define MAX_CAMERAS		(MAX_NETIF * MAX_CAMERAS_PER_NETIF)

// Enable/disable Bayer to RGB conversion
// (If disabled - Bayer format will be treated as Monochrome).
#define ENABLE_BAYER_CONVERSION 1

// Enable/disable buffer FULL/EMPTY handling (cycling)
#define USE_SYNCHRONOUS_BUFFER_CYCLING	0

// Enable/disable transfer tuning (buffering, timeouts, thread affinity).
#define TUNE_STREAMING_THREADS 0

#define NUM_BUF	8
void *m_latestBuffer = NULL;

typedef struct tagMY_CONTEXT
{
   X_VIEW_HANDLE     View;
	GEV_CAMERA_HANDLE camHandle;
	int					depth;
	int 					format;
	void 					*convertBuffer;
	BOOL					convertFormat;
	BOOL              exit;
}MY_CONTEXT, *PMY_CONTEXT;


X_VIEW_HANDLE  View = NULL;
PUINT8 bufAddress[NUM_BUF];
GEV_CAMERA_HANDLE handle = NULL;
MY_CONTEXT context = {0};
GEV_STATUS status;
int numBuffers = NUM_BUF;
UINT32 maxHeight = 1600;
UINT32 maxWidth = 2048;
UINT32 maxDepth = 2;
UINT64 allocate_size;
int type;
int turboDriveAvailable = 0;
UINT32 height = 0;
UINT32 width = 0;
UINT32 format = 0;
char uniqueName[128];

static unsigned long us_timer_init( void )
{
   struct timeval tm;
   unsigned long msec;
   
   // Get the time and turn it into a millisecond counter.
   gettimeofday( &tm, NULL);
   
   msec = (tm.tv_sec * 1000000) + (tm.tv_usec);
   return msec;
}
static unsigned long ms_timer_init( void )
{
   struct timeval tm;
   unsigned long msec;
   
   // Get the time and turn it into a millisecond counter.
   gettimeofday( &tm, NULL);
   
   msec = (tm.tv_sec * 1000) + (tm.tv_usec / 1000);
   return msec;
}

static int ms_timer_interval_elapsed( unsigned long origin, unsigned long timeout)
{
   struct timeval tm;
   unsigned long msec;
   
   // Get the time and turn it into a millisecond counter.
   gettimeofday( &tm, NULL);
   
   msec = (tm.tv_sec * 1000) + (tm.tv_usec / 1000);
      
   // Check if the timeout has expired.
   if ( msec > origin )
   {
      return ((msec - origin) >= timeout) ? TRUE : FALSE;
   }
   else
   {
      return ((origin - msec) >= timeout) ? TRUE : FALSE;
   }
}

static void _GetUniqueFilename( char *filename, size_t size, char *basename)
{
	// Create a filename based on the current time (to 0.01 seconds)
	struct timeval tm;
	uint32_t years, days, hours, seconds;

	if ((filename != NULL) && (basename != NULL) )
	{
		if (size > (16 + sizeof(basename)) )
		{
	
			// Get the time and turn it into a 10 msec resolution counter to use as an index.
			gettimeofday( &tm, NULL);
			years = ((tm.tv_sec / 86400) / 365);
			tm.tv_sec = tm.tv_sec - (years*86400*365);
			days  = (tm.tv_sec / 86400);
			tm.tv_sec = tm.tv_sec - (days * 86400);
			hours = (tm.tv_sec / 3600);
			seconds = tm.tv_sec - (hours * 3600);						
															
			snprintf(filename, size, "%s_%03d%02d%04d%02d", basename, days,hours, (int)seconds, (int)(tm.tv_usec/10000));
		}
	}
}


char GetKey()
{
   char key = getchar();
   while ((key == '\r') || (key == '\n'))
   {
      key = getchar();
   }
   return key;
}

void PrintMenu()
{
   printf("GRAB CTL : [S]=stop, [1-9]=snap N, [G]=continuous, [A]=Abort\n");
   printf("MISC     : [Q]or[ESC]=end,         [T]=Toggle TurboMode (if available), [@]=SaveToFile\n");
}

void * ImageDisplayThread( void *context)
{
	MY_CONTEXT *displayContext = (MY_CONTEXT *)context;

	if (displayContext != NULL)
	{
   	unsigned long prev_time = 0;
   	//unsigned long cur_time = 0;
		//unsigned long deltatime = 0;
		prev_time = us_timer_init();

		// While we are still running.
		while(!displayContext->exit)
		{
			GEV_BUFFER_OBJECT *img = NULL;
			GEV_STATUS status = 0;
	
			// Wait for images to be received
			status = GevWaitForNextImage(displayContext->camHandle, &img, 1000);

			if ((img != NULL) && (status == GEVLIB_OK))
			{
				if (img->status == 0)
				{
					m_latestBuffer = img->address;
					// Can the acquired buffer be displayed?
					if ( IsGevPixelTypeX11Displayable(img->format) || displayContext->convertFormat )
					{
						// Convert the image format if required.
						if (displayContext->convertFormat)
						{
							int gev_depth = GevGetPixelDepthInBits(img->format);
							// Convert the image to a displayable format.
							//(Note : Not all formats can be displayed properly at this time (planar, YUV*, 10/12 bit packed).
							ConvertGevImageToX11Format( img->w, img->h, gev_depth, img->format, img->address, \
													displayContext->depth, displayContext->format, displayContext->convertBuffer);
					
							// Display the image in the (supported) converted format. 
							Display_Image( displayContext->View, displayContext->depth, img->w, img->h, displayContext->convertBuffer );				
						}
						else
						{
							// Display the image in the (supported) received format. 
							Display_Image( displayContext->View, img->d,  img->w, img->h, img->address );
						}
					}
					else
					{
						//printf("Not displayable\n");
					}
				}
				else
				{
					// Image had an error (incomplete (timeout/overflow/lost)).
					// Do any handling of this condition necessary.
				}
			}
#if USE_SYNCHRONOUS_BUFFER_CYCLING
			if (img != NULL)
			{
				// Release the buffer back to the image transfer process.
				GevReleaseImage( displayContext->camHandle, img);
			}
#endif
		}
	}
	pthread_exit(0);	
}

int IsTurboDriveAvailable(GEV_CAMERA_HANDLE handle)
{
	int type;
	UINT32 val = 0;
	
	if ( 0 == GevGetFeatureValue( handle, "transferTurboCurrentlyAbailable",  &type, sizeof(UINT32), &val) )
	{
		// Current / Standard method present - this feature indicates if TurboMode is available.
		// (Yes - it is spelled that odd way on purpose).
		return (val != 0);
	}
	else
	{
		// Legacy mode check - standard feature is not there try it manually.
		char pxlfmt_str[64] = {0};

		// Mandatory feature (always present).
		GevGetFeatureValueAsString( handle, "PixelFormat", &type, sizeof(pxlfmt_str), pxlfmt_str);

		// Set the "turbo" capability selector for this format.
		if ( 0 != GevSetFeatureValueAsString( handle, "transferTurboCapabilitySelector", pxlfmt_str) )
		{
			// Either the capability selector is not present or the pixel format is not part of the 
			// capability set.
			// Either way - TurboMode is NOT AVAILABLE.....
			return 0; 
		}
		else
		{
			// The capabilty set exists so TurboMode is AVAILABLE.
			// It is up to the camera to send TurboMode data if it can - so we let it.
			return 1;
		}
	}
	return 0;
}



int CameraStart()
{
	GEV_DEVICE_INTERFACE  pCamera[MAX_CAMERAS] = {0};
	int numCamera = 0;
	int camIndex = 0;
    pthread_t  tid;
	//char c;
	//int done = FALSE;
	uint32_t macLow = 0; // Low 32-bits of the mac address (for file naming).
	
	//============================================================================
	// Greetings
	printf ("\nGigE Vision Library GenICam C++ Example Program (%s)\n", __DATE__);
	printf ("Copyright (c) 2015, DALSA.\nAll rights reserved.\n\n");

	//===================================================================================
	// Set default options for the library.
	{
		GEVLIB_CONFIG_OPTIONS options = {0};

		GevGetLibraryConfigOptions( &options);
		//options.logLevel = GEV_LOG_LEVEL_OFF;
		//options.logLevel = GEV_LOG_LEVEL_TRACE;
		options.logLevel = GEV_LOG_LEVEL_NORMAL;
		GevSetLibraryConfigOptions( &options);
	}

	//====================================================================================
	// DISCOVER Cameras
	//
	// Get all the IP addresses of attached network cards.

	status = GevGetCameraList( pCamera, MAX_CAMERAS, &numCamera);

	printf ("%d camera(s) on the network\n", numCamera);

	// Select the first camera found (unless the command line has a parameter = the camera index)
	if (numCamera != 0)
	{
		// if (argc > 1)
		// {
		// 	sscanf(argv[1], "%d", &camIndex);
		// 	if (camIndex >= (int)numCamera)
		// 	{
		// 		printf("Camera index out of range - only %d camera(s) are present\n", numCamera);
		// 		camIndex = -1;
		// 	}
		// }

		if (camIndex != -1)
		{
			//====================================================================
			// Connect to Camera
			//
			//
			int i;
			UINT64 payload_size;
			UINT32 pixFormat = 0;
			UINT32 pixDepth = 0;
			UINT32 convertedGevFormat = 0;
			
			//====================================================================
			// Open the camera.
			status = GevOpenCamera( &pCamera[camIndex], GevExclusiveMode, &handle);
			if (status == 0)
			{
				//=================================================================
				// GenICam feature access via Camera XML File enabled by "open"
				// 
				// Get the name of XML file name back (example only - in case you need it somewhere).
				//
				char xmlFileName[MAX_PATH] = {0};
				status = GevGetGenICamXML_FileName( handle, (int)sizeof(xmlFileName), xmlFileName);
				if (status == GEVLIB_OK)
				{
					printf("XML stored as %s\n", xmlFileName);
				}
				status = GEVLIB_OK;
			}
			// Get the low part of the MAC address (use it as part of a unique file name for saving images).
			// Generate a unique base name to be used for saving image files
			// based on the last 3 octets of the MAC address.
			macLow = pCamera[camIndex].macLow;
			macLow &= 0x00FFFFFF;
			snprintf(uniqueName, sizeof(uniqueName), "img_%06x", macLow); 
			
			
			// Go on to adjust some API related settings (for tuning / diagnostics / etc....).
			if ( status == 0 )
			{
				GEV_CAMERA_OPTIONS camOptions = {0};

				// Adjust the camera interface options if desired (see the manual)
				GevGetCameraInterfaceOptions( handle, &camOptions);
				//camOptions.heartbeat_timeout_ms = 60000;		// For debugging (delay camera timeout while in debugger)
				camOptions.heartbeat_timeout_ms = 5000;		// Disconnect detection (5 seconds)

#if TUNE_STREAMING_THREADS
				// Some tuning can be done here. (see the manual)
				camOptions.streamFrame_timeout_ms = 1001;				// Internal timeout for frame reception.
				camOptions.streamNumFramesBuffered = 4;				// Buffer frames internally.
				camOptions.streamMemoryLimitMax = 64*1024*1024;		// Adjust packet memory buffering limit.	
				camOptions.streamPktSize = 9180;							// Adjust the GVSP packet size.
				camOptions.streamPktDelay = 10;							// Add usecs between packets to pace arrival at NIC.
				
				// Assign specific CPUs to threads (affinity) - if required for better performance.
				{
					int numCpus = _GetNumCpus();
					if (numCpus > 1)
					{
						camOptions.streamThreadAffinity = numCpus-1;
						camOptions.serverThreadAffinity = numCpus-2;
					}
				}
#endif
				// Write the adjusted interface options back.
				GevSetCameraInterfaceOptions( handle, &camOptions);

				//=====================================================================
				// Get the GenICam FeatureNodeMap object and access the camera features.
				GenApi::CNodeMapRef *Camera = static_cast<GenApi::CNodeMapRef*>(GevGetFeatureNodeMap(handle));
				
				if (Camera)
				{
					// Access some features using the bare GenApi interface methods
					try 
					{
						//Mandatory features....
						GenApi::CIntegerPtr ptrIntNode = Camera->_GetNode("Width");
						width = (UINT32) ptrIntNode->GetValue();
						ptrIntNode = Camera->_GetNode("Height");
						height = (UINT32) ptrIntNode->GetValue();
						ptrIntNode = Camera->_GetNode("PayloadSize");
						payload_size = (UINT64) ptrIntNode->GetValue();
						GenApi::CEnumerationPtr ptrEnumNode = Camera->_GetNode("PixelFormat") ;
						format = (UINT32)ptrEnumNode->GetIntValue();
					}
					// Catch all possible exceptions from a node access.
					CATCH_GENAPI_ERROR(status);
				}

				if (status == 0)
				{
					//=================================================================
					// Set up a grab/transfer from this camera
					//
					printf("Camera ROI set for \n\tHeight = %d\n\tWidth = %d\n\tPixelFormat (val) = 0x%08x\n", height,width,format);

					maxHeight = height;
					maxWidth = width;
					maxDepth = GetPixelSizeInBytes(GevGetUnpackedPixelType(format));

					// Allocate image buffers (adjusting for any unpacking of packed pixels)
					// (Either the image size or the payload_size, whichever is larger).
					allocate_size = maxDepth * maxWidth * maxHeight;
					allocate_size = (payload_size > allocate_size) ? payload_size : allocate_size;
					for (i = 0; i < numBuffers; i++)
					{
						bufAddress[i] = (PUINT8)malloc(allocate_size);
						memset(bufAddress[i], 0, allocate_size);

					}

#if USE_SYNCHRONOUS_BUFFER_CYCLING
					// Initialize a transfer with synchronous buffer handling.
					status = GevInitializeTransfer( handle, SynchronousNextEmpty, allocate_size, numBuffers, bufAddress);
#else
					// Initialize a transfer with asynchronous buffer handling.
					status = GevInitializeTransfer( handle, Asynchronous, allocate_size, numBuffers, bufAddress);
#endif

					// Create an image display window.
					// This works best for monochrome and RGB. The packed color formats (with Y, U, V, etc..) require 
					// conversion as do, if desired, Bayer formats.
					// (Packed pixels are unpacked internally unless passthru mode is enabled).

					// Translate the raw pixel format to one suitable for the (limited) Linux display routines.			

					status = GetX11DisplayablePixelFormat( ENABLE_BAYER_CONVERSION, format, &convertedGevFormat, &pixFormat);

					if (format != convertedGevFormat) 
					{
						// We MAY need to convert the data on the fly to display it.
						if (GevIsPixelTypeRGB(convertedGevFormat))
						{
							// Conversion to RGB888 required.
							pixDepth = 32;	// Assume 4 8bit components for color display (RGBA)
							context.format = Convert_SaperaFormat_To_X11( pixFormat);
							context.depth = pixDepth;
							context.convertBuffer = malloc((maxWidth * maxHeight * ((pixDepth + 7)/8)));
							context.convertFormat = TRUE;
						}
						else
						{
							// Converted format is MONO - generally this is handled
							// internally (unpacking etc...) unless in passthru mode.
							// (						
							pixDepth = GevGetPixelDepthInBits(convertedGevFormat);
							context.format = Convert_SaperaFormat_To_X11( pixFormat);
							context.depth = pixDepth;							
							context.convertBuffer = NULL;
							context.convertFormat = FALSE;
						}
					}
					else
					{
						pixDepth = GevGetPixelDepthInBits(convertedGevFormat);
						context.format = Convert_SaperaFormat_To_X11( pixFormat);
						context.depth = pixDepth;
						context.convertBuffer = NULL;
						context.convertFormat = FALSE;
					}
					
					View = CreateDisplayWindow("GigE-V GenApi Console Demo", TRUE, height, width, pixDepth, pixFormat, FALSE ); 

					// Create a thread to receive images from the API and display them.
					context.View = View;
					context.camHandle = handle;
					context.exit = FALSE;
		   		pthread_create(&tid, NULL, ImageDisplayThread, &context); 

          // Call the main command loop or the example.
          PrintMenu();

	        return 0;
#if 0
          while(!done)
          {
		        c = GetKey();

		            // Toggle turboMode
            if ((c == 'T') || (c=='t'))
            {
							// See if TurboDrive is available.
							turboDriveAvailable = IsTurboDriveAvailable(handle);
							if (turboDriveAvailable)
							{
								UINT32 val = 1;
								GevGetFeatureValue(handle, "transferTurboMode", &type, sizeof(UINT32), &val);
								val = (val == 0) ? 1 : 0;
								GevSetFeatureValue(handle, "transferTurboMode", sizeof(UINT32), &val);
								GevGetFeatureValue(handle, "transferTurboMode", &type, sizeof(UINT32), &val);
								if (val == 1)
								{
									printf("TurboMode Enabled\n"); 	
								}
								else
								{
									printf("TurboMode Disabled\n"); 	
								}														
							}
							else
							{
								printf("*** TurboDrive is NOT Available for this device/pixel format combination ***\n");
							}
            }

		            // Stop
            if ((c == 'S') || (c=='s') || (c == '0'))
            {
							GevStopTransfer(handle);
            }
		            //Abort
            if ((c == 'A') || (c=='a'))
            {
	 						GevAbortTransfer(handle);
						}
		            // Snap N (1 to 9 frames)
            if ((c >= '1')&&(c<='9'))
            {
							for (i = 0; i < numBuffers; i++)
							{
								memset(bufAddress[i], 0, allocate_size);
							}

							status = GevStartTransfer( handle, (UINT32)(c-'0'));
							if (status != 0) printf("Error starting grab - 0x%x  or %d\n", status, status); 
						}
		        // Continuous grab.
            if ((c == 'G') || (c=='g'))
            {
							for (i = 0; i < numBuffers; i++)
							{
								memset(bufAddress[i], 0, allocate_size);
							}
	 						status = GevStartTransfer( handle, -1);
							if (status != 0) printf("Error starting grab - 0x%x  or %d\n", status, status); 
            }
					
		            // Save image
            if ((c == '@'))
		        {
							char filename[128] = {0};
							int ret = -1;
							uint32_t saveFormat = format;
							void *bufToSave = m_latestBuffer;
							int allocate_conversion_buffer = 0;
							
							// Make sure we have data to save.
							if ( m_latestBuffer != NULL )
							{
								uint32_t component_count = 1;
								UINT32 convertedFmt = 0;
								
								// Bayer conversion enabled for save image to file option.
								//
								// Get the converted pixel type received from the API that is 
								//	based on the pixel type output from the camera.
								// (Packed formats are automatically unpacked - unless in "passthru" mode.)
								//
								convertedFmt = GevGetConvertedPixelType( 0, format);
								
								if ( GevIsPixelTypeBayer( convertedFmt ) && ENABLE_BAYER_CONVERSION )
								{
									int img_size = 0;
									int img_depth = 0;
									uint8_t fill = 0;
									
									// Bayer will be converted to RGB.
									saveFormat = GevGetBayerAsRGBPixelType(convertedFmt);
									
									// Convert the image to RGB.
									img_depth = GevGetPixelDepthInBits(saveFormat);
									component_count = GevGetPixelComponentCount(saveFormat);
									img_size = width * height * component_count* ((img_depth + 7)/8);
									bufToSave = malloc(img_size);  
									fill = (component_count == 4) ? 0xFF : 0;  // Alpha if needed.
									memset( bufToSave, fill, img_size);
									allocate_conversion_buffer = 1;
									
									// Convert the Bayer to RGB	
									ConvertBayerToRGB( 0, height, width, convertedFmt, m_latestBuffer, saveFormat, bufToSave);

								}
								else
								{
									saveFormat = convertedFmt;
									allocate_conversion_buffer = 0;
								}
								
								// Generate a file name from the unique base name.
								_GetUniqueFilename(filename, (sizeof(filename)-5), uniqueName);
								
#if defined(LIBTIFF_AVAILABLE)
								// Add the file extension we want.
								strncat( filename, ".tif", sizeof(filename));
								
								// Write the file (from the latest buffer acquired).
								ret = Write_GevImage_ToTIFF( filename, width, height, saveFormat, bufToSave);								
								if (ret > 0)
								{
									printf("Image saved as : %s : %d bytes written\n", filename, ret); 
								}
								else
								{
									printf("Error %d saving image\n", ret);
								}
#else
								printf("*** Library libtiff not installed ***\n");
#endif
							}
							else
							{
								printf("No image buffer has been acquired yet !\n");
							}
							
							if (allocate_conversion_buffer)
							{
								free(bufToSave);
							}
						
		        }
		        if (c == '?')
		        {
	           PrintMenu();
		        }

		        if ((c == 0x1b) || (c == 'q') || (c == 'Q'))
		        {
							GevStopTransfer(handle);
		          done = TRUE;
							context.exit = TRUE;
		   				pthread_join( tid, NULL);      
		        }
		      }
#endif
					GevAbortTransfer(handle);
					status = GevFreeTransfer(handle);
					DestroyDisplayWindow(View);


					for (i = 0; i < numBuffers; i++)
					{	
						free(bufAddress[i]);
					}
					if (context.convertBuffer != NULL)
					{
						free(context.convertBuffer);
						context.convertBuffer = NULL;
					}
				}
				GevCloseCamera(&handle);
			}
			else
			{
				printf("Error : 0x%0x : opening camera\n", status);
			}
		}
	}

    // Close down the API.
    GevApiUninitialize();

    // Close socket API
    _CloseSocketAPI ();	// must close API even on error


	//printf("Hit any key to exit\n");
	//kbhit();

}


void CameraStop()
{
  int i;
  GevAbortTransfer(handle);
  status = GevFreeTransfer(handle);
  DestroyDisplayWindow(View);

  for (i = 0; i < numBuffers; i++)
  {	
    free(bufAddress[i]);
  }
  if (context.convertBuffer != NULL)
  {
    free(context.convertBuffer);
    context.convertBuffer = NULL;
  }
  GevCloseCamera(&handle); 
  // Close down the API.
  GevApiUninitialize();

  // Close socket API
  _CloseSocketAPI ();	// must close API even on error        
  gtk_main_quit();
}

// void button_clicked5(GtkWidget * widget, gpointer data)
// {
//   // See if TurboDrive is available.
//   turboDriveAvailable = IsTurboDriveAvailable(handle);
//   if (turboDriveAvailable)
//   {
//     UINT32 val = 1;
//     GevGetFeatureValue(handle, "transferTurboMode", &type, sizeof(UINT32), &val);
//     val = (val == 0) ? 1 : 0;
//     GevSetFeatureValue(handle, "transferTurboMode", sizeof(UINT32), &val);
//     GevGetFeatureValue(handle, "transferTurboMode", &type, sizeof(UINT32), &val);
//     if (val == 1)
//     {
//       printf("TurboMode Enabled\n"); 	
//     }
//     else
//     {
//       printf("TurboMode Disabled\n"); 	
//     }														
//   }
//   else
//   {
//     printf("*** TurboDrive is NOT Available for this device/pixel format combination ***\n");
//   }
// }

// void button_clicked6(GtkWidget * widget, gpointer data)
// {
//   int i;
//   for (i = 0; i < numBuffers; i++)
//   {
//     memset(bufAddress[i], 0, allocate_size);
//   }

//   status = GevStartTransfer( handle, (UINT32)1);
//   if (status != 0) printf("Error starting grab - 0x%x  or %d\n", status, status); 
// }

void button_clicked7(GtkWidget * widget, gpointer data)
{
  char filename[128] = {0};
  int ret = -1;
  uint32_t saveFormat = format;
  void *bufToSave = m_latestBuffer;
  int allocate_conversion_buffer = 0;

  // Make sure we have data to save.
  if ( m_latestBuffer != NULL )
  {
    uint32_t component_count = 1;
    UINT32 convertedFmt = 0;
    
    // Bayer conversion enabled for save image to file option.
    //
    // Get the converted pixel type received from the API that is 
    //	based on the pixel type output from the camera.
    // (Packed formats are automatically unpacked - unless in "passthru" mode.)
    //
    convertedFmt = GevGetConvertedPixelType( 0, format);
    
    if ( GevIsPixelTypeBayer( convertedFmt ) && ENABLE_BAYER_CONVERSION )
    {
      int img_size = 0;
      int img_depth = 0;
      uint8_t fill = 0;
      
      // Bayer will be converted to RGB.
      saveFormat = GevGetBayerAsRGBPixelType(convertedFmt);
      
      // Convert the image to RGB.
      img_depth = GevGetPixelDepthInBits(saveFormat);
      component_count = GevGetPixelComponentCount(saveFormat);
      img_size = width * height * component_count* ((img_depth + 7)/8);
      bufToSave = malloc(img_size);  
      fill = (component_count == 4) ? 0xFF : 0;  // Alpha if needed.
      memset( bufToSave, fill, img_size);
      allocate_conversion_buffer = 1;
      
      // Convert the Bayer to RGB	
      ConvertBayerToRGB( 0, height, width, convertedFmt, m_latestBuffer, saveFormat, bufToSave);

    }
    else
    {
      saveFormat = convertedFmt;
      allocate_conversion_buffer = 0;
    }
    
    // Generate a file name from the unique base name.
    _GetUniqueFilename(filename, (sizeof(filename)-5), uniqueName);
    
  #if defined(LIBTIFF_AVAILABLE)
    // Add the file extension we want.
    strncat( filename, ".tif", sizeof(filename));
    
    // Write the file (from the latest buffer acquired).
    ret = Write_GevImage_ToTIFF( filename, width, height, saveFormat, bufToSave);								
    if (ret > 0)
    {
      printf("Image saved as : %s : %d bytes written\n", filename, ret); 
    }
    else
    {
      printf("Error %d saving image\n", ret);
    }
  #else
    printf("*** Library libtiff not installed ***\n");
  #endif
  }
  else
  {
    printf("No image buffer has been acquired yet !\n");
  }

  if (allocate_conversion_buffer)
  {
    free(bufToSave);
  }
}

class ExampleWindow : public Gtk::Window
{
public:
  ExampleWindow();
  virtual ~ExampleWindow();

protected:
  //Signal handlers:
  void on_checkbutton_toggled();
  void on_combo_position();
  void on_adjustment1_value_changed();
  void on_adjustment2_value_changed();
  void on_button_quit();
  void on_button_clicked();
  void on_button_clicked2();
  void on_button_clicked3();
  void on_button_clicked4();
  void on_button_clicked5();
  void on_button_clicked6();


  //Child widgets:
  Gtk::Box m_VBox_Top, m_VBox2, m_VBox_HScale;
  Gtk::Box m_HBox_Scales, m_HBox_Combo, m_HBox_Digits, m_HBox_PageSize;

  Glib::RefPtr<Gtk::Adjustment> m_adjustment, m_adjustment_digits, m_adjustment_pagesize;

  Gtk::Scale m_VScale;
  Gtk::Scale m_HScale, m_Scale_Digits, m_Scale_PageSize;

  Gtk::Separator m_Separator;
  Gtk::Separator m_Separator2;
  Gtk::Separator m_Separator3;
  Gtk::Separator m_Separator4;
  Gtk::Separator m_Separator5;

  Gtk::CheckButton m_CheckButton;

  Gtk::Scrollbar m_Scrollbar;

  //Tree model columns:
  class ModelColumns : public Gtk::TreeModel::ColumnRecord
  {
  public:

    ModelColumns()
    { add(m_col_position_type); add(m_col_title); }

    Gtk::TreeModelColumn<Gtk::PositionType> m_col_position_type;
    Gtk::TreeModelColumn<Glib::ustring> m_col_title;
  };

  ModelColumns m_Columns;

  //Child widgets:
  Gtk::ComboBox m_ComboBox_Position;
  Glib::RefPtr<Gtk::ListStore> m_refTreeModel;

  Gtk::Button m_Button_Quit;
  Gtk::Button m_button;
  Gtk::Button m_button2;
  Gtk::Button m_button3;
  Gtk::Button m_button4;
  Gtk::Button m_button5;
  Gtk::Button m_button6;


  //Child widgets:
  Gtk::Box m_VBox;

  Gtk::ScrolledWindow m_ScrolledWindow;
  Gtk::TextView m_TextView;

  Glib::RefPtr<Gtk::TextBuffer> m_refTextBuffer1, m_refTextBuffer2;
};

int main(int argc, char *argv[])
{
  auto app = Gtk::Application::create(argc, argv, "org.gtkmm.example");

  ExampleWindow window;

  //Shows the window and returns when it is closed.
  return app->run(window);
}

ExampleWindow::ExampleWindow()
:
  m_VBox_Top(Gtk::ORIENTATION_VERTICAL, 0),
  m_VBox2(Gtk::ORIENTATION_VERTICAL, 10),
  m_VBox_HScale(Gtk::ORIENTATION_VERTICAL, 10),
  m_HBox_Scales(Gtk::ORIENTATION_HORIZONTAL, 10),
  m_HBox_Combo(Gtk::ORIENTATION_HORIZONTAL, 10),
  m_HBox_Digits(Gtk::ORIENTATION_HORIZONTAL, 10),
  m_HBox_PageSize(Gtk::ORIENTATION_HORIZONTAL, 10),

  // Value, lower, upper, step_increment, page_increment, page_size:
  // Note that the page_size value only makes a difference for
  // scrollbar widgets, and the highest value you'll get is actually
  // (upper - page_size).
  m_adjustment( Gtk::Adjustment::create(0.0, 0.0, 101.0, 0.1, 1.0, 1.0) ),
  m_adjustment_digits( Gtk::Adjustment::create(1.0, 0.0, 5.0, 1.0, 2.0) ),
  m_adjustment_pagesize( Gtk::Adjustment::create(1.0, 1.0, 101.0) ),

  m_VScale(m_adjustment, Gtk::ORIENTATION_VERTICAL),
  m_HScale(m_adjustment, Gtk::ORIENTATION_HORIZONTAL),
  m_Scale_Digits(m_adjustment_digits),
  m_Scale_PageSize(m_adjustment_pagesize),

  // A checkbutton to control whether the value is displayed or not:
  m_CheckButton("Display value on scale widgets", 0),

  // Reuse the same adjustment again.
  // Notice how this causes the scales to always be updated
  // continuously when the scrollbar is moved.
  m_Scrollbar(m_adjustment),

  m_Button_Quit("Quit"),
  m_button("CameraInitialize"),
  m_button2("CameraFinalize"),
  m_button3("GetCameraInterfaceOptions"),
  m_button4("Constant Grab"),
  m_button5("Constant Grab Stop"),
  m_button6("One Shot")
{
  set_title("range controls");
  set_default_size(300, 900);

  //VScale:
  m_VScale.set_digits(1);
  m_VScale.set_value_pos(Gtk::POS_TOP);
  m_VScale.set_draw_value();
  m_VScale.set_inverted(); // highest value at top

  //HScale:
  m_HScale.set_digits(1);
  m_HScale.set_value_pos(Gtk::POS_TOP);
  m_HScale.set_draw_value();

  add(m_VBox_Top);
  m_VBox_Top.pack_start(m_VBox2);
  m_VBox2.set_border_width(10);
  m_VBox2.pack_start(m_HBox_Scales);

  //Put VScale and HScale (above scrollbar) side-by-side.
  m_HBox_Scales.pack_start(m_VScale);
  m_HBox_Scales.pack_start(m_VBox_HScale);

  m_VBox_HScale.pack_start(m_HScale);

  //Scrollbar:
  m_VBox_HScale.pack_start(m_Scrollbar);

  //CheckButton:
  m_CheckButton.set_active();
  m_CheckButton.signal_toggled().connect( sigc::mem_fun(*this,
    &ExampleWindow::on_checkbutton_toggled) );
  m_VBox2.pack_start(m_CheckButton, Gtk::PACK_SHRINK);

  //Position ComboBox:
  //Create the Tree model:
  m_refTreeModel = Gtk::ListStore::create(m_Columns);
  m_ComboBox_Position.set_model(m_refTreeModel);
  m_ComboBox_Position.pack_start(m_Columns.m_col_title);

  //Fill the ComboBox's Tree Model:
  Gtk::TreeModel::Row row = *(m_refTreeModel->append());
  row[m_Columns.m_col_position_type] = Gtk::POS_TOP;
  row[m_Columns.m_col_title] = "Top";
  row = *(m_refTreeModel->append());
  row[m_Columns.m_col_position_type] = Gtk::POS_BOTTOM;
  row[m_Columns.m_col_title] = "Bottom";
  row = *(m_refTreeModel->append());
  row[m_Columns.m_col_position_type] = Gtk::POS_LEFT;
  row[m_Columns.m_col_title] = "Left";
  row = *(m_refTreeModel->append());
  row[m_Columns.m_col_position_type] = Gtk::POS_RIGHT;
  row[m_Columns.m_col_title] = "Right";

  m_VBox2.pack_start(m_HBox_Combo, Gtk::PACK_SHRINK);
  m_HBox_Combo.pack_start(
    *Gtk::make_managed<Gtk::Label>("Scale Value Position:", 0), Gtk::PACK_SHRINK);
  m_HBox_Combo.pack_start(m_ComboBox_Position);
  m_ComboBox_Position.signal_changed().connect( sigc::mem_fun(*this, &ExampleWindow::on_combo_position) );
  m_ComboBox_Position.set_active(0); // Top

  //Digits:
  m_HBox_Digits.pack_start(
    *Gtk::make_managed<Gtk::Label>("Scale Digits:", 0), Gtk::PACK_SHRINK);
  m_Scale_Digits.set_digits(0);
  m_adjustment_digits->signal_value_changed().connect(sigc::mem_fun(*this,
    &ExampleWindow::on_adjustment1_value_changed));
  m_HBox_Digits.pack_start(m_Scale_Digits);

  //Page Size:
  m_HBox_PageSize.pack_start(
    *Gtk::make_managed<Gtk::Label>("Scrollbar Page Size:", 0),
    Gtk::PACK_SHRINK);
  m_Scale_PageSize.set_digits(0);
  m_adjustment_pagesize->signal_value_changed().connect(sigc::mem_fun(*this,
    &ExampleWindow::on_adjustment2_value_changed));
  m_HBox_PageSize.pack_start(m_Scale_PageSize);

  m_VBox2.pack_start(m_HBox_Digits, Gtk::PACK_SHRINK);
  m_VBox2.pack_start(m_HBox_PageSize, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_Separator, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_button, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_Separator2, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_button3, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_Separator3, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_button2, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_Separator4, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_button4, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_button5, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_button6, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_Separator5, Gtk::PACK_SHRINK);
  m_VBox_Top.pack_start(m_Button_Quit, Gtk::PACK_SHRINK);
  

  m_Button_Quit.set_can_default();
  m_Button_Quit.grab_default();
  m_Button_Quit.signal_clicked().connect(sigc::mem_fun(*this,
    &ExampleWindow::on_button_quit));
  m_Button_Quit.set_border_width(10);

  m_button.set_can_default();
  m_button.grab_default();
  //m_button.add_pixlabel("info.xpm", "cool button");

  m_button.signal_clicked().connect( sigc::mem_fun(*this,
              &ExampleWindow::on_button_clicked) );
  m_button.set_border_width(10);


  m_button2.set_can_default();
  m_button2.grab_default();
  //m_button.add_pixlabel("info.xpm", "cool button");

  m_button2.signal_clicked().connect( sigc::mem_fun(*this,
              &ExampleWindow::on_button_clicked2) );
  m_button2.set_border_width(10);


  m_button3.set_can_default();
  m_button3.grab_default();
  //m_button.add_pixlabel("info.xpm", "cool button");

  m_button3.signal_clicked().connect( sigc::mem_fun(*this,
              &ExampleWindow::on_button_clicked3) );
  m_button3.set_border_width(10);


  m_button4.set_can_default();
  m_button4.grab_default();
  //m_button.add_pixlabel("info.xpm", "cool button");

  m_button4.signal_clicked().connect( sigc::mem_fun(*this,
              &ExampleWindow::on_button_clicked4) );
  m_button4.set_border_width(10);


  m_button5.set_can_default();
  m_button5.grab_default();
  //m_button.add_pixlabel("info.xpm", "cool button");

  m_button5.signal_clicked().connect( sigc::mem_fun(*this,
              &ExampleWindow::on_button_clicked5) );
  m_button5.set_border_width(10);

  m_button6.set_can_default();
  m_button6.grab_default();
  //m_button.add_pixlabel("info.xpm", "cool button");

  m_button6.signal_clicked().connect( sigc::mem_fun(*this,
              &ExampleWindow::on_button_clicked6) );
  m_button6.set_border_width(10);

  //Add the TreeView, inside a ScrolledWindow, with the button underneath:
  m_ScrolledWindow.add(m_TextView);

  //Only show the scrollbars when they are necessary:
  m_ScrolledWindow.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

  m_VBox2.pack_start(m_ScrolledWindow);

  show_all_children();
}

ExampleWindow::~ExampleWindow()
{
}

void ExampleWindow::on_checkbutton_toggled()
{
  m_VScale.set_draw_value(m_CheckButton.get_active());
  m_HScale.set_draw_value(m_CheckButton.get_active());
}

void ExampleWindow::on_combo_position()
{
  Gtk::TreeModel::iterator iter = m_ComboBox_Position.get_active();
  if(!iter)
    return;

  Gtk::TreeModel::Row row = *iter;
  if(!row)
    return;

  const Gtk::PositionType postype = row[m_Columns.m_col_position_type];

  m_VScale.set_value_pos(postype);
  m_HScale.set_value_pos(postype);
}

void ExampleWindow::on_adjustment1_value_changed()
{
  const double val = m_adjustment_digits->get_value();
  m_VScale.set_digits((int)val);
  m_HScale.set_digits((int)val);
}

void ExampleWindow::on_adjustment2_value_changed()
{
  const double val = m_adjustment_pagesize->get_value();
  m_adjustment->set_page_size(val);
  m_adjustment->set_page_increment(val);

  // Note that we don't have to emit the "changed" signal
  // because gtkmm does this for us.
}

void ExampleWindow::on_button_quit()
{
  hide();
}

void ExampleWindow::on_button_clicked()
{
  CameraStart();
  std::cout << "The Button was clicked." << std::endl;
}

void ExampleWindow::on_button_clicked2()
{
  CameraStop();
  std::cout << "The Button was clicked." << std::endl;
}

void ExampleWindow::on_button_clicked3()
{
  GEV_CAMERA_OPTIONS camOptions = {0};
  GevGetCameraInterfaceOptions( handle, &camOptions);

  
  m_refTextBuffer1 = Gtk::TextBuffer::create();
  //m_refTextBuffer1->set_text(std::to_string(camOptions.numRetries));
  //m_refTextBuffer1->insert(m_refTextBuffer1->get_end_iter(), "And this is a newly inserted line\n");
  m_TextView.set_buffer(m_refTextBuffer1);

  std::ostringstream oss;

  oss << camOptions.numRetries << std::endl;
  oss << camOptions.command_timeout_ms << std::endl;
  oss << camOptions.heartbeat_timeout_ms << std::endl;
  oss << camOptions.streamPktSize << std::endl;
  oss << camOptions.streamPktDelay << std::endl;
  oss << camOptions.streamNumFramesBuffered << std::endl;
  oss << camOptions.streamMemoryLimitMax << std::endl;
  oss << camOptions.streamMaxPacketResends << std::endl;
  oss << camOptions.streamFrame_timeout_ms << std::endl;
  oss << camOptions.streamThreadAffinity << std::endl;
  oss << camOptions.serverThreadAffinity << std::endl;
  oss << camOptions.msgChannel_timeout_ms << std::endl;
  oss << camOptions.enable_passthru_mode;

  m_refTextBuffer1->set_text(oss.str());
  m_TextView.set_buffer(m_refTextBuffer1);
  
}

void ExampleWindow::on_button_clicked4()
{
  int i;
  // Continuous grab.
  for (i = 0; i < numBuffers; i++)
  {
    memset(bufAddress[i], 0, allocate_size);
  }
  status = GevStartTransfer(handle, -1);
  if (status != 0) printf("Error starting grab - 0x%x  or %d\n", status, status);
}

void ExampleWindow::on_button_clicked5()
{
  // Continuous grab Stop.
  GevStopTransfer(handle);
}


void ExampleWindow::on_button_clicked6()
{
  // One Shot.
  int i;
  for (i = 0; i < numBuffers; i++)
  {
    memset(bufAddress[i], 0, allocate_size);
  }

  status = GevStartTransfer( handle, (UINT32)1);
  if (status != 0) printf("Error starting grab - 0x%x  or %d\n", status, status); 
}