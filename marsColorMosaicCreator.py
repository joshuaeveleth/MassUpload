
import os
import sys
import re
import subprocess
import numpy
import copy
import multiprocessing
import threading
import logging
import datetime

import IrgGeoFunctions
#import copyGeoTiffInfo
import mosaicTileManager # TODO: Normalize caps!
import MosaicUtilities
import hrscImageManager
import hrscFileCacher

"""

Existing tools:
- RegisterHrsc.cpp
    - Input  = Basemap, HRSC
    - Output = spatialTransform
- writeHrscColorPairs.cpp
    - Input  = Basemap, HRSC, spatialTransform
    - Output = File containing pixel color pairs
- transformHrscImageColor.cpp
    - Input  = HRSC, colorTransform
    - Output = Color transformed HRSC image
    - TODO   = Add cleanup/pansharp


"""

#----------------------------------------------------------------------------
# Constants

# TODO: Go down to 20 to 10 meters!
OUTPUT_RESOLUTION_METERS_PER_PIXEL = 100

# 
NUM_DOWNLOAD_THREADS = 5 # There are five files we download per data set
NUM_PROCESS_THREADS  = 6

# --> Downloading the HRSC files seems to be the major bottleneck.

# Set up the log path here.
# - Log tiles are timestamped as is each line in the log file
currentTime = datetime.datetime.now()
logPath = ('/byss/smcmich1/data/hrscMosaicLogs/hrscMosaicLog_%s.txt' % currentTime.isoformat() )
logging.basicConfig(filename=logPath,
                    format='%(asctime)s %(name)s %(message)s',
                    level=logging.DEBUG)



#-----------------------------------------------------------------------------------------
# Functions



def cacheManagerThreadFunction(databasePath, outputFolder, inputQueue, outputQueue):
    '''Thread to allow downloading of HRSC data in parallel with image processing.
       The input queue recieves three types of commands:
           "STOP" --> Finish current tasks, then exit.
           "KILL" --> Immediately kill all the threads and exit.
           "FETCH data_set_name" --> Fetch the specified data set
           "FINISHED data_set_name" --> Signals that this data set can be safely deleted
'''

    logger = logging.getLogger('DownloadThread')

    # Initialize a process pool to be managed by this thread
    downloadPool = None
    if NUM_DOWNLOAD_THREADS > 1:
        downloadPool = multiprocessing.Pool(processes=NUM_DOWNLOAD_THREADS)

    # Set up the HRSC file manager object
    print 'Initializing HRSC file caching object'
    hrscFileFetcher = hrscFileCacher.HrscFileCacher(databasePath, outputFolder, downloadPool)

    while True:

        # Fetch the next requested data set from the input queue
        request = inputQueue.get() 

        # Handle stop request
        if request == 'STOP':
            logger.info('Download thread manager received stop request, stopping download threads...')
            # Gracefully wait for current work to finish
            if downloadPool:
                downloadPool.close()
                downloadPool.join()  
            break
            
        if request == 'KILL':
            logger.info('Download thread manager received kill request, killing download threads...')
            # Immediately stop all work
            if downloadPool:
                downloadPool.terminate()
                downloadPool.join()  
            break
      
        if 'FETCH' in request:
            dataSet = request[len('FETCH'):].strip()
            logger.info('Got request to fetch data set ' + dataSet)
            # Download this HRSC image using the thread pool
            # - TODO: Allow download overlap of multiple data sets at once!
            hrscInfoDict = hrscFileFetcher.fetchHrscDataSet(dataSet)
            logger.info('Finished fetching data set ' + dataSet)
            # Put the output information on the output queue
            outputQueue.put(hrscInfoDict)

   
        
        #--> Need to make sure we never delete an image until we are finished using it

        # TODO: Implement 'finished' message?

    # We only get here when we break out of the main loop
    outputQueue.put('STOPPED')
    logger.info('Download manager thread stopped.')


# For debugging only, the hrscFileCacher class does the actual call from the database.
def getHrscImageList():
    '''For just returns a fixed list of HRSC images for testing'''   
    return ['h0022_0000',
            'h0506_0000',
            'h2411_0000']#,
            #'h6419_0000']




def getCoveredOutputTiles(basemapInstance, hrscInstance):
    '''Return a bounding box containing all the output tiles covered by the HRSC image'''
    
    hrscBoundingBoxDegrees = hrscInstance.getBoundingBoxDegrees()
    return basemapInstance.getIntersectingTiles(hrscBoundingBoxDegrees)
    #return MosaicUtilities.Rectangle(196, 197, 92, 93) # DEBUG


def getHrscTileUpdateDict(basemapInstance, tileIndex, hrscInstance):
    '''Gets the dictionary of HRSC tiles that need to update the given basemap tile index'''

    thisTileBounds = basemapInstance.getTileRectDegree(tileIndex)

    # Get the tile information from the HRSC image
    tileDict = hrscInstance.getTileInfo(thisTileBounds, tileIndex.getPostfix())
    print 'Found these tile intersections:'
    for hrscTile in tileDict.itervalues():
        print hrscTile['prefix']

    return tileDict
    
    

def updateTileWithHrscImage(hrscTileInfoDict, outputTilePath, tileLogPath):
    '''Update a single output tile with the given HRSC image'''

    # TODO: This C++ program can do multiple tiles in one call.

    # For each tile...
    for hrscTile in hrscTileInfoDict.itervalues():    
        #try:
        cmd = ('./hrscMosaic ' + outputTilePath +' '+ outputTilePath +' '+ hrscTile['newColorPath'] +' '+
                                  hrscTile['tileMaskPath'] +' '+ hrscTile['tileToTileTransformPath'])
        MosaicUtilities.cmdRunner(cmd, outputTilePath, True)
        #raise Exception('DEBUG')

    # Return the path to log the success to
    return tileLogPath
    
    


def updateTilesContainingHrscImage(basemapInstance, hrscInstance, pool=None):
    '''Updates all output tiles containing this HRSC image'''

    logger = logging.getLogger('MainProgram')

    # Find all the output tiles that intersect with this
    outputTilesRect = getCoveredOutputTiles(basemapInstance, hrscInstance)

    hrscSetName = hrscInstance.getSetName()
    mainLogPath = basemapInstance.getMainLogPath()
    
    # Skip this function if we have completed adding this HRSC image
    if basemapInstance.checkLog(mainLogPath, hrscSetName):
        print 'Have already completed adding HRSC image ' + hrscSetName + ',  skipping it.'
        return
    
    logger.info('Started updating tiles for HRSC image ' + hrscSetName)

    print 'Found overlapping output tiles:  ' + str(outputTilesRect)
    if pool:
        print 'Initializing tile output tasks...'
    
    # Loop through all the tiles
    tileResults = []
    for row in range(outputTilesRect.minY, outputTilesRect.maxY):
        for col in range(outputTilesRect.minX, outputTilesRect.maxX):
    
            # Set up the til information
            tileIndex  = MosaicUtilities.TileIndex(row, col) #basemapInstance.getTileIndex(98.5, -27.5)
            tileBounds = basemapInstance.getTileRectDegree(tileIndex)
            
            print 'Using HRSC image ' + hrscSetName + ' to update tile: ' + str(tileIndex)
            print '--> Tile bounds = ' + str(tileBounds)

            print '\nMaking sure basemap info is present...'
            
            # Now that we have selected a tile, generate all of the tile images for it.
            (smallTilePath, largeTilePath, grayTilePath, outputTilePath, tileLogPath) =  \
                        basemapInstance.generateTileImages(tileIndex, False)
        
            #print '\nPasting on HRSC tiles...'

            # Have we already written this HRSC image to this tile?
            comboAlreadyWritten = basemapInstance.checkLog(tileLogPath, hrscSetName)
            if comboAlreadyWritten:
                print '-- Skipping already written tile!' #Don't want to double-write the same image.
                continue
        
            # Get information about which HRSC tiles to paste on to the basemap
            hrscTileInfoDict = getHrscTileUpdateDict(basemapInstance, tileIndex, hrscInstance)
            if not hrscTileInfoDict: # If there are no tiles to use, move on to the next output tile!
                continue
        
            # Update the selected tile with the HRSC image
            if pool:
                # Send the function and arguments to the thread pool
                dictCopy = copy.copy(hrscTileInfoDict)
                tileResults.append(pool.apply_async(updateTileWithHrscImage,
                                                    args=(dictCopy, outputTilePath, tileLogPath)))
            else: # Just run the function
                updateTileWithHrscImage(hrscTileInfoDict, outputTilePath, tileLogPath)
            
            # DEBUG breaks
            #break
        #break


    if pool: # Wait for all the tasks to complete
        print 'Finished initializing tile output tasks.'
        print 'Waiting for tile processes to complete...'
        for result in tileResults:
            # Each task finishes by returning the log path for that tile.
            # - Record that we have used this HRSC/tile combination.
            # - This requires that tiles with no HRSC tiles do not get assigned a task.
            tileLogPath = result.get()
            basemapInstance.updateLog(tileLogPath, hrscSetName)
            
            
        print 'All tile writing processes have completed'

    #raise Exception('DEBUG')
        
    # Log the fact that we have finished adding this HRSC image    
    basemapInstance.updateLog(mainLogPath, hrscSetName)
    
    print '\n---> Finished updating tiles for HRSC image ' + hrscSetName
    logger.info('Finished updating tiles for HRSC image ' + hrscSetName)

#-----------------------------------------------------------------------------------------

# Laptop
#testDirectory    = '/home/smcmich1/data/hrscMapTest/'
#fullBasemapPath  = testDirectory + 'projection_space_basemap.tif'
#sourceHrscFolder = testDirectory + 'external_data'
#hrscOutputFolder = testDirectory + 'hrscFiles'
#outputTileFolder = testDirectory + 'outputTiles'
#databasePath     = 'FAIL'

# Lunokhod 2
fullBasemapPath  = '/byss/smcmich1/data/hrscBasemap/projection_space_basemap.tif'
sourceHrscFolder = '/home/smcmich1/data/hrscDownloadCache'
hrscOutputFolder = '/home/smcmich1/data/hrscProcessedFiles'
outputTileFolder = '/byss/smcmich1/data/hrscBasemap/outputTiles'
databasePath     = '/byss/smcmich1/data/google/googlePlanetary.db'

print 'Starting basemap enhancement script...'

logger = logging.getLogger('MainProgram')

# Initialize the multi-threading worker pools
# - Seperate pools for downloads and processing
#downloadPool = None
processPool  = None
#if NUM_DOWNLOAD_THREADS > 1:
#    downloadPool = multiprocessing.Pool(processes=NUM_DOWNLOAD_THREADS)
if NUM_PROCESS_THREADS > 1:
    processPool = multiprocessing.Pool(processes=NUM_PROCESS_THREADS)

print '\n==== Initializing the base map object ===='
basemapInstance = mosaicTileManager.MarsBasemap(fullBasemapPath, outputTileFolder, OUTPUT_RESOLUTION_METERS_PER_PIXEL)
mainLogPath = basemapInstance.getMainLogPath()
print '--- Finished initializing the base map object ---\n'



# Get a list of all the HRSC images we are testing with
#fullImageList = getHrscImageList()
tempFileFinder = hrscFileCacher.HrscFileCacher(databasePath, sourceHrscFolder)
fullImageList = tempFileFinder.getAllHrscSetList()
tempFileFinder = None # Delet this temporary object

print len(fullImageList)
print fullImageList[:10]

raise Exception('DEBUG!')

# Prune out all the HRSC images that we have already added to the mosaic.
hrscImageList = []
for hrscSetName in fullImageList:
    if basemapInstance.checkLog(mainLogPath, hrscSetName):
        print 'Have already completed adding HRSC image ' + hrscSetName + ',  skipping it.'
    else:
        hrscImageList.append(hrscSetName)

print 'image list = ' + str(hrscImageList)



## Set up the HRSC file manager object
print 'Starting communication queues'
#hrscFileFetcher = hrscFileCacher.HrscFileCacher(databasePath, sourceHrscFolder, downloadPool)
downloadCommandQueue  = multiprocessing.Queue()
downloadResponseQueue = multiprocessing.Queue()
print 'Initializing HRSC file caching thread'
downloadThread = threading.Thread(target=cacheManagerThreadFunction,
                                  args  =(databasePath, sourceHrscFolder,            
                                          downloadCommandQueue, downloadResponseQueue)
                                 )
downloadThread.daemon = True # Needed for ctrl-c to work
print 'Running thread...'
downloadThread.start()


# Go ahead and send a request to fetch the first HRSC image
logger.info('Sending FETCH command: ' + hrscImageList[0])
downloadCommandQueue.put('FETCH ' + hrscImageList[0])


# Loop through input HRSC images
numHrscDataSets = len(hrscImageList) 
for i in range(0,numHrscDataSets): 
    
    # Get the name of this and the next data set
    hrscSetName = hrscImageList[i]
    nextSetName = None
    if i < numHrscDataSets-1:
        nextSetName = hrscImageList[i+1]
        # Go ahead and submit the fetch request for the next set name.
        logger.info('Sending FETCH command: ' + nextSetName)
        downloadCommandQueue.put('FETCH ' + nextSetName)

    # Notes on downloading:
    # - Each iteration of this loop commands one download, and waits for one download.
    # - The queues keep things in order, and the download thread handles one data set at a time.
    # - This means that we can have one data set downloading while one data set is being processed.
    # - The next improvement to be made would be to download multiple data sets at the same time.

   
    ## Pick a location to store the data for this HRSC image
    thisHrscFolder = os.path.join(hrscOutputFolder, hrscSetName)

    #try:

    print '\n=== Fetching HRSC image ' + hrscSetName + ' ==='

    # Fetch the HRSC data from the web
    #hrscFileInfoDict = hrscFileFetcher.fetchHrscDataSet(hrscSetName)
    hrscFileInfoDict = downloadResponseQueue.get() # Wait for the parallel thread to provide the data
    if not 'setName' in hrscFileInfoDict:
        raise Exception('Ran out of HRSC files, processing stopped!!!')
    if hrscFileInfoDict['setName'] != hrscSetName:
        raise Exception('Set fetch mismatch!  Expected %s, got %s instead!' % 
                         (hrscSetName, hrscFileInfoDict['setName']))
    logger.info('Received fetch information for ' + hrscSetName)

    #print 'SKIPPING IMAGE PROCESSING!!!'
    #continue

    print '\n=== Initializing HRSC image ' + hrscSetName + ' ==='

    # Preprocess the HRSC image
    hrscInstance = hrscImageManager.HrscImage(hrscFileInfoDict, thisHrscFolder, basemapInstance, False, processPool)

    # TODO: Need to make the HRSC manager clean up the processed folder too!

    print '--- Now initializing high res HRSC content ---'

    # Complete the high resolution components
    hrscInstance.prepHighResolutionProducts()
    
    print '--- Finished initializing HRSC image ---\n'

    # Call the function to update all the output images for this HRSC image
    updateTilesContainingHrscImage(basemapInstance, hrscInstance, processPool)

    print '<<<<< Finished writing all tiles for this HRSC image! >>>>>'

    # TODO: Clean up if necessary

    #raise Exception('DEBUG')


if processPool:
    print 'Cleaning up the processing thread pool...'
    processPool.close()
    processPool.join()

downloadCommandQueue.put('STOP') # Stop the download thread
downloadThread.join()
#if downloadPool:
#    print 'Cleaning up the download thread pool...'
#    downloadPool.close()
#    downloadPool.join()

print 'Basemap enhancement script completed!'

