/*
	Write inversion table session to a file.
	
	Originally gyro_accelerometer_tutorial03.c
	-------------------------------------------------------------------

	This program  reads the angles from the accelerometer and gyroscope
	on a BerryIMU connected to a Raspberry Pi.
	http://ozzmaker.com/
	

    Copyright (C) 2014  Mark Williams

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    Library General Public License for more details.
    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
    MA 02111-1307, USA
*/



#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "sensor.c"

// GLOBAL CONFIGURATION

//#define DT 0.10         // [s/loop] loop period. 20ms
float DT = 0.10;
int SECONDS_TO_SETTLE = 15;
const char PATH[] = "../../messages/";

#define AA 0.97         // complementary filter constant

#define A_GAIN 0.0573      // [deg/LSB]
#define G_GAIN 0.070     // [deg/s/LSB]
#define RAD_TO_DEG 57.29578
#define M_PI 3.14159265358979323846


//Used by Kalman Filters
float Q_angle  =  0.01;
float Q_gyro   =  0.0003;
float R_angle  =  0.01;
float x_bias = 0;
float y_bias = 0;
float XP_00 = 0, XP_01 = 0, XP_10 = 0, XP_11 = 0;
float YP_00 = 0, YP_01 = 0, YP_10 = 0, YP_11 = 0;
float KFangleX = 0.0;
float KFangleY = 0.0;

float kalmanFilterX(float accAngle, float gyroRate);
float kalmanFilterY(float accAngle, float gyroRate);
int tableStateFromAngle(float tableAngle);


void  INThandler(int sig)
{
        signal(sig, SIG_IGN);
        exit(0);
}

int mymillis()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec) * 1000 + (tv.tv_usec)/1000;
}

int timeval_subtract(struct timeval *result, struct timeval *t2, struct timeval *t1)
{
    long int diff = (t2->tv_usec + 1000000 * t2->tv_sec) - (t1->tv_usec + 1000000 * t1->tv_sec);
    result->tv_sec = diff / 1000000;
    result->tv_usec = diff % 1000000;
    return (diff<0);
}

int epochtime()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec;
}

int main(int argc, char *argv[])
{
	float rate_gyr_y = 0.0;   // [deg/s]
	float rate_gyr_x = 0.0;    // [deg/s]
	float rate_gyr_z = 0.0;     // [deg/s]

	int  accRaw[3];
	int  magRaw[3];
	int  gyrRaw[3];

	float gyroXangle = 0.0;
	float gyroYangle = 0.0;
	float gyroZangle = 0.0;
	float AccYangle = 0.0;
	float AccXangle = 0.0;
	float CFangleX = 0.0;
	float CFangleY = 0.0;

	int startInt  = mymillis();
	struct  timeval tvBegin, tvEnd,tvDiff;
	
    signal(SIGINT, INThandler);

	enableIMU();

	gettimeofday(&tvBegin, NULL);

	// TableMessageWriter
	int tableState = 0;
	int prevTableState = 0;
	int tableStateChangeCount = 0;
	int bootInt = mymillis();
	int isCFSettled = 0;
	int settledState;
	
	int sessionStartTime = mymillis();
	int isInSession;
	
	char sessionAsJson[50000];
	char messageStateString[3];
	char messageTimeString[6];
	char messageFilePath[255];
	char messageFileName[255];
	char tableMessage[50000];
	
	while(1)
	{
		startInt = mymillis();
		
		//read ACC and GYR data
		readACC(accRaw);
		readGYR(gyrRaw);

		//Convert Gyro raw to degrees per second
		rate_gyr_x = (float) gyrRaw[0]  * G_GAIN;
		rate_gyr_y = (float) gyrRaw[1]  * G_GAIN;
		rate_gyr_z = (float) gyrRaw[2]  * G_GAIN;

		//Calculate the angles from the gyro
		gyroXangle+=rate_gyr_x*DT;
		gyroYangle+=rate_gyr_y*DT;
		gyroZangle+=rate_gyr_z*DT;

		//Convert Accelerometer values to degrees
		AccXangle = (float) (atan2(accRaw[1],accRaw[2])+M_PI)*RAD_TO_DEG;
		AccYangle = (float) (atan2(accRaw[2],accRaw[0])+M_PI)*RAD_TO_DEG;

			//Change the rotation value of the accelerometer to -/+ 180 and move the Y axis '0' point to up.
			//Two different pieces of code are used depending on how your IMU is mounted.
			//If IMU is upside down
		/*
			if (AccXangle >180)
					AccXangle -= (float)360.0;

			AccYangle-=90;
			if (AccYangle >180)
					AccYangle -= (float)360.0;
		*/

			//If IMU is up the correct way, use these lines
			AccXangle -= (float)180.0;
		if (AccYangle > 90)
				AccYangle -= (float)270;
		else
			AccYangle += (float)90;

		//Kalman Filter
		float kalmanX = kalmanFilterX(AccXangle, rate_gyr_x);
		float kalmanY = kalmanFilterY(AccYangle, rate_gyr_y);
		//printf ("\033[22;31mkalmanX %7.3f  \033[22;36mkalmanY %7.3f\t\e[m",kalmanX,kalmanY);

		//Complementary filter used to combine the accelerometer and gyro values.
		CFangleX=AA*(CFangleX+rate_gyr_x*DT) +(1 - AA) * AccXangle;
		CFangleY=AA*(CFangleY+rate_gyr_y*DT) +(1 - AA) * AccYangle;

		//printf ("GyroX  %7.3f \t AccXangle \e[m %7.3f \t \033[22;31mCFangleX %7.3f\033[0m\t GyroY  %7.3f \t AccYangle %7.3f \t \033[22;36mCFangleY %7.3f\t\033[0m\n",gyroXangle,AccXangle,CFangleX,gyroYangle,AccYangle,CFangleY);
		printf ("CFangleY %7.3f\t\033[0m",CFangleY);
		
		prevTableState = tableState;
		tableState = tableStateFromAngle(CFangleY);
		
		printf ("tableState:[%3d]",tableState);

		// Settle the complementary filter on bootInt
		if (!isCFSettled && (startInt - bootInt) > (SECONDS_TO_SETTLE*1000))
		{
			isCFSettled = 1;
			settledState = tableState;
			printf ("<-- *****STATE HAS SETTLED WITH TABLE STATE:[%3d]*****", tableState);
			isInSession = 0;
		}
		
		// On Table State Change
		if (prevTableState != tableState && isCFSettled)
		{
			printf("\"tableState\":%d", tableState-settledState);
			printf("\"time\":%d,",mymillis() - sessionStartTime);
			
						
			// Session Logic - NOW IN SESSION
			if (!isInSession && (tableState != settledState) && (tableState > settledState))
			{
				isInSession = 1;
				sessionStartTime = mymillis();
				printf("<-- STARTING INVERSION TABLE SESSION");
				strcpy(sessionAsJson, "{");

			}
			// NOW OUT OF SESSION
			else if (isInSession && tableState == settledState)
			{
				isInSession = 0;
				printf("<-- OUT OF INVERSION TABLE SESSION");
				
				// remove the last comma
				sessionAsJson[strlen(sessionAsJson)-1] = 0;
				strcat(sessionAsJson, "}");
				
				//printf("\n\n%s\n\n",sessionAsJson); // print sessionAsJson
				
				// determine filepath to save
				//sprintf(messageFilePath, "%s", PATH);
				strcpy(messageFilePath, "");
				strcat(messageFilePath, PATH);
				sprintf(messageFileName,"YIZITIAN_%d", epochtime());
				strcat(messageFilePath, messageFileName);
				
				// build table message to write
				sprintf(tableMessage, "{\"SESSION_TIME\":%d,\"SESSION\":%s}",mymillis(),sessionAsJson);
				printf("\nWriting message:[%s] to path:[%s]",tableMessage,messageFilePath);
				
				// save sessionAsJson to file
				FILE * messageOnDisk;
				messageOnDisk = fopen(messageFilePath, "ab");
				fprintf(messageOnDisk, tableMessage);
				fclose(messageOnDisk);
				printf("\nTable message written to disk\n");
				
				// clear sessionAsJson
				strcpy(sessionAsJson,"");
			}

			// Log duration if in session
			if (isInSession)
			{
				strcat(sessionAsJson, "{\"TABLE_STATE\":");
				sprintf(messageStateString, "%d", tableState-settledState);
				strcat(sessionAsJson, messageStateString);
				strcat(sessionAsJson, ", \"TIME\":");
				sprintf(messageTimeString, "%d", mymillis()-sessionStartTime);
				strcat(sessionAsJson, messageTimeString);
				strcat(sessionAsJson, "},");
			}

		}
		
		printf ("\n");
		//Each loop should be at least 20ms.
			while(mymillis() - startInt < (DT*1000))
			{
				usleep(100);
			}
		printf("Time %d\t", mymillis());
		//printf("Loop Time %d\t", mymillis()- startInt);
    } // while
}

  int tableStateFromAngle(float tableAngle)
  {
	int stateBucketInDegrees = 30;
    int result;
	result = (int) floor(nearbyintf(tableAngle) / stateBucketInDegrees);
    return result;
  }

  float kalmanFilterX(float accAngle, float gyroRate)
  {
    float  y, S;
    float K_0, K_1;


    KFangleX += DT * (gyroRate - x_bias);

    XP_00 +=  - DT * (XP_10 + XP_01) + Q_angle * DT;
    XP_01 +=  - DT * XP_11;
    XP_10 +=  - DT * XP_11;
    XP_11 +=  + Q_gyro * DT;

    y = accAngle - KFangleX;
    S = XP_00 + R_angle;
    K_0 = XP_00 / S;
    K_1 = XP_10 / S;

    KFangleX +=  K_0 * y;
    x_bias  +=  K_1 * y;
    XP_00 -= K_0 * XP_00;
    XP_01 -= K_0 * XP_01;
    XP_10 -= K_1 * XP_00;
    XP_11 -= K_1 * XP_01;

    return KFangleX;
  }


  float kalmanFilterY(float accAngle, float gyroRate)
  {
    float  y, S;
    float K_0, K_1;


    KFangleY += DT * (gyroRate - y_bias);

    YP_00 +=  - DT * (YP_10 + YP_01) + Q_angle * DT;
    YP_01 +=  - DT * YP_11;
    YP_10 +=  - DT * YP_11;
    YP_11 +=  + Q_gyro * DT;

    y = accAngle - KFangleY;
    S = YP_00 + R_angle;
    K_0 = YP_00 / S;
    K_1 = YP_10 / S;

    KFangleY +=  K_0 * y;
    y_bias  +=  K_1 * y;
    YP_00 -= K_0 * YP_00;
    YP_01 -= K_0 * YP_01;
    YP_10 -= K_1 * YP_00;
    YP_11 -= K_1 * YP_01;

    return KFangleY;
  }

