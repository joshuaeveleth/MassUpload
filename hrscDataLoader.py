#!/usr/bin/env python
# __BEGIN_LICENSE__
#  Copyright (c) 2009-2013, United States Government as represented by the
#  Administrator of the National Aeronautics and Space Administration. All
#  rights reserved.
#
#  The NGT platform is licensed under the Apache License, Version 2.0 (the
#  "License"); you may not use this file except in compliance with the
#  License. You may obtain a copy of the License at
#  http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
# __END_LICENSE__

import sys

from BeautifulSoup import BeautifulSoup

import os, glob, optparse, re, shutil, subprocess, string, time, urllib, urllib2

import multiprocessing

import mapsEngineUpload, IrgStringFunctions, IrgGeoFunctions

import common


#--------------------------------------------------------------------------------

def getUploadList(fileList):
    '''Returns the subset of the fileList that needs to be uploaded to GME'''
    return fileList[0] # Only the converted tif file needs to be uploaded

def getCreationTime(fileList):
    """Extract the file creation time and return in YYYY-MM-DDTHH:MM:SSZ format"""
    
    # We get this from the original non-tif file 
    if len(fileList) < 2:
        raise Exception('Error, missing original file path!')
    filePath = fileList[1]
    
    # Use subprocess to parse the command output
    cmd = ['gdalinfo', filePath]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    cmdOut, err = p.communicate()

    # Find the time string in the text
    timeString = IrgStringFunctions.getLineAfterText(cmdOut, 'PRODUCT_CREATION_TIME=')
    
    # Get to the correct format
    timeString = timeString.strip()
    timeString = timeString[:-5] + 'Z'
    
    return timeString
    
def getBoundingBox(fileList):
    """Return the bounding box for this data set in the format (minLon, maxLon, minLat, maxLat)"""
    return IrgGeoFunctions.getImageBoundingBox(fileList[0])

def findAllDataSets(db, sensorCode):
    '''Add all known data sets to the SQL database'''
    
    print 'Updating HRSC PDS image data list...'
          
    baseUrl    = "http://pds-geosciences.wustl.edu/mex/mex-m-hrsc-5-refdr-mapprojected-v2/mexhrsc_1001/data/"
    baseDemUrl = "http://pds-geosciences.wustl.edu/mex/mex-m-hrsc-5-refdr-dtm-v1/mexhrs_2001/data/"
    
    # Parse the top PDS level
    parsedIndexPage = BeautifulSoup(urllib2.urlopen((baseUrl)).read())
    
    # Loop through outermost directory
    for line in parsedIndexPage.findAll('a'):
        
        # Skip links we are not interested in
        if len(line.string.strip()) != 4:
            continue
        
        dataPrefix = 'h' + line.string
        
        subFolderUrl = baseUrl + line.string + '/'
        parsedDataPage = BeautifulSoup(urllib2.urlopen((subFolderUrl)).read())
        
        # Loop through the data files
        # - There is a core set of files on each page but there can be
        #   more with incremented image numbers.
        for d in parsedDataPage.findAll('a'):
            if '[' in d.string: # Skip some bad links
                continue

            dataFileName = d.string[:-4]     # Lop off the .img portion
            subtype      = dataFileName[-3:] # Extract the type

            # Easier to just call this function to get the URL
            url = generatePdsPath(dataFileName)

            common.addDataRecord(db, sensorCode, subtype, dataFileName, url)

    print 'Updating HRSC PDS DEM data list...'

    # Now parse the top PDS DEM level
    parsedIndexPage = BeautifulSoup(urllib2.urlopen((baseDemUrl)).read())
    
    # Loop through outermost directory
    for line in parsedIndexPage.findAll('a'):
        
        # Skip links we are not interested in
        if len(line.string.strip()) != 4:
            continue
        
        subFolderUrl   = baseUrl + line.string + '/'
        parsedDataPage = BeautifulSoup(urllib2.urlopen((subFolderUrl)).read())
        
        # Loop through the data files and look for DEMs
        for d in parsedDataPage.findAll('a'):
            
            if '_dt4.img' not in d.string: # This is the DEM extension
                continue

            setName = d.string[:-4]     # Lop off the .img portion
            url = subFolderUrl +  d.string

            common.addDataRecord(db, sensorCode, 'DEM', setName, url)
            
    
def fetchAndPrepFile(setName, subtype, remoteURL, workDir):
    '''Retrieves a remote file and prepares it for upload'''
    # The same code here works for images and DEMs
    
    #print 'Uploading file ' + remoteURL
        
    localFileName = os.path.splitext(os.path.basename(remoteURL))[0]+'.tif'
    localFilePath = os.path.join(workDir, localFileName)
    downloadPath  = os.path.join(workDir, os.path.basename(remoteURL))
    
    if not os.path.exists(localFilePath):
        # Download the file
        cmd = 'wget ' + remoteURL + ' -O ' + downloadPath
        print cmd
        os.system(cmd)
    
    # Convert to GTiff format
    cmd = 'gdal_translate -of GTiff ' + downloadPath + ' ' + localFilePath
    print cmd
    os.system(cmd)
    
    # Insert a delay here to make sure that the tiny files do not get uploaded too fast for Google.
    # - For the larger files this will barely be noticable.
    time.sleep(5)

    # TODO: Is meta information still in the DEM source file?
    return [localFilePath, downloadPath]
    

#--------------------------------------------------------------------------------





# fileType is the file name after the prefix
def generatePdsPath(filePrefix):
    """Generate the full PDS path for a given HRSC data file"""
       
    # File prefix looks like this: hHXXX_DDDD_SSS
    fileType = '.img'
    
    # Extract the run number --> HXXX
    runNum = filePrefix[1:5]
       
    filename = filePrefix + fileType
    baseUrl  = "http://pds-geosciences.wustl.edu/mex/mex-m-hrsc-5-refdr-mapprojected-v2/mexhrsc_1001/data/"
    fullUrl  = baseUrl + runNum +"/"+ filename

    #print filePrefix + fileType + ' -> ' + fullUrl
    return fullUrl


