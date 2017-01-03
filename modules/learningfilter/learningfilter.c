/*
 * Created on: Dec, 2016
 * Author: dongchen@ini.uzh.ch
 */

#include "learningfilter.h"
#include "base/mainloop.h"
#include "base/module.h"
#include "ext/buffers.h"
#include "libcaer/devices/dynapse.h"
#include <math.h>

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <signal.h>

struct LFilter_state {
	caerInputDynapseState eventSourceModuleState; //sshsNode // caerInputDynapseState // need to be carefully tested
	sshsNode eventSourceConfigNode;
	int32_t colorscaleMax;
	int32_t colorscaleMin;
	int8_t reset;
	float resetExProbability;
	int8_t resetExType;
	int8_t resetInType;
	double learningRateForward;
	double learningRateBackward;
	int16_t dataSizeX;
	int16_t dataSizeY;
	int16_t visualizerSizeX;
	int16_t visualizerSizeY;
	int16_t apsSizeX;
	int16_t apsSizeY;
	bool stimulate;
	bool learning;
	int32_t maxSynapseFeature;
	int32_t maxSynapseOutput;
};

struct LFilter_memory {
	simple2DBufferInt connectionMap;	//store all the connections, size: TOTAL_NEURON_NUM_ON_CHIP by TOTAL_NEURON_NUM_ON_CHIP
	simple2DBufferInt connectionCamMap;	//store the CAM id for each pre-post neurons pair
	simple2DBufferInt camMap;			//the CAMs are available or not
	simple2DBufferInt camSize;			//available CAM
	simple2DBufferInt sramMap;			//the SRAMs are available or not
	simple2DBufferInt sramMapContent;	//the chipId + coreId information for each SRAM
	simple2DBufferDouble weightMap;		//store all the weights
	simple2DBufferInt synapseMap;		//store all the synapses
	simple2DBufferLong spikeFifo;		//FIFO for storing all the events
	simple2DBufferInt filterMap;		//store the pre-neuron address for each filter
	simple2DBufferInt camMapContentSource;	//store all the CAM content for each filter
	simple2DBufferInt camMapContentType;	//store all the synapse type for each filter
	simple2DBufferInt filterMapSize;	//size for every filter
	simple2DBufferInt outputMap;
	simple2DBufferInt outputMapDisabled;
//	simple2DBufferInt outputMapSize;
//	simple2DBufferInt inhibitoryValid;
//	simple2DBufferInt inhibitoryVirtualNeuronAddr;
	uint64_t spikeCounter;				//number of spikes in the FIFO
	uint64_t preRdPointer;				//pre-read pointer for the FIFO
	uint64_t wrPointer;					//write pointer for the FIFO
};

double deltaWeights[DELTA_WEIGHT_LUT_LENGTH];
double synapseUpgradeThreshold[SYNAPSE_UPGRADE_THRESHOLD_LUT_LENGTH];

typedef struct {
	uint16_t r,g,b;
} COLOUR;

typedef struct LFilter_state *LFilterState;
typedef struct LFilter_memory LFilterMemory; // *LFilterMemory

static LFilterMemory memory;
static int8_t reseted = 0;
static int8_t teachingSignalDisabled = 0;
//static int8_t stimulating = 0;
static int time_count = 0; //static int64_t time_count = 0;
static int time_count_last = 0; //static int64_t time_count_last = 0; //-1; //0;
static int32_t stimuliPattern = 0;
static struct itimerval oldtv;

static bool caerLearningFilterInit(caerModuleData moduleData); //It may not run at the beginning of the experiment ????????????
static void caerLearningFilterRun(caerModuleData moduleData, size_t argsNumber, va_list args);
static void caerLearningFilterConfig(caerModuleData moduleData);
static void caerLearningFilterExit(caerModuleData moduleData);
static void caerLearningFilterReset(caerModuleData moduleData, uint16_t resetCallSourceID);
static void ModifyForwardSynapse(caerModuleData moduleData, int16_t eventSourceID, int64_t preNeuronAddr, int64_t postNeuronAddr, double deltaWeight, caerFrameEventPacket *synapseplotfeatureA, caerFrameEventPacket *weightplotfeatureA);
static void ModifyBackwardSynapse(caerModuleData moduleData, int64_t preNeuronAddr, int64_t postNeuronAddr, double deltaWeight, caerFrameEventPacket *weightplotfeatureA);
static bool ResetNetwork(caerModuleData moduleData, int16_t eventSourceID);
static bool BuildSynapse(caerModuleData moduleData, int16_t eventSourceID, uint32_t preNeuronAddr, uint32_t postNeuronAddr, uint32_t virtualNeuronAddr, int16_t synapseType, int8_t realOrVirtualSynapse, int8_t virtualNeuronAddrEnable);
static bool WriteCam(caerModuleData moduleData, int16_t eventSourceID, uint32_t preNeuronAddr, uint32_t postNeuronAddr, uint32_t virtualNeuronAddr, uint32_t camId, int16_t synapseType, int8_t virtualNeuronAddrEnable);
static bool WriteSram(caerModuleData moduleData, int16_t eventSourceID, uint32_t preNeuronAddr, uint32_t postNeuronAddr, uint32_t virtualNeuronAddr, uint32_t sramId, int8_t virtualNeuronAddrEnable);
static void Shuffle1DArray(int64_t *array, int64_t Range);
static void GetRand1DArray(int64_t *array, int64_t Range, int64_t CamNumAvailable);
static void GetRand1DBinaryArray(int64_t *binaryArray, int64_t Range, int64_t CamNumAvailable);
static bool ResetBiases(caerModuleData moduleData, int16_t eventSourceID);
static bool EnableStimuliGen(caerModuleData moduleData, int16_t eventSourceID, int32_t pattern);
static bool DisableStimuliGen(caerModuleData moduleData, int16_t eventSourceID);
static bool EnableStimuliGenPrimitiveCam(caerModuleData moduleData, int16_t eventSourceID);
static bool DisableStimuliGenPrimitiveCam(caerModuleData moduleData, int16_t eventSourceID);
static bool EnableTeachingSignal(caerModuleData moduleData, int16_t eventSourceID);
static bool DisableTeachingSignal(caerModuleData moduleData, int16_t eventSourceID);
static bool EnableTeaching(caerModuleData moduleData, int16_t eventSourceID);
static bool DisableTeaching(caerModuleData moduleData, int16_t eventSourceID);
static bool SetInputLayerCam(caerModuleData moduleData, int16_t eventSourceID);
static bool ClearAllCam(caerModuleData moduleData, int16_t eventSourceID);
static void setBiasBits(caerModuleData moduleData, int16_t eventSourceID, uint32_t chipId, uint32_t coreId, const char *biasName_t,
		uint8_t coarseValue, uint16_t fineValue, const char *lowHigh, const char *npBias);
static void SetTimer(void);
static void SignalHandler(int m);

COLOUR GetColourW(double v, double vmin, double vmax);
COLOUR GetColourS(int v); //, double vmin, double vmax

static struct caer_module_functions caerLearningFilterFunctions = { .moduleInit =
	&caerLearningFilterInit, .moduleRun = &caerLearningFilterRun, .moduleConfig =
	&caerLearningFilterConfig, .moduleExit = &caerLearningFilterExit, .moduleReset =
	&caerLearningFilterReset };

void caerLearningFilter(uint16_t moduleID, int16_t eventSourceID, caerSpikeEventPacket spike,
		caerFrameEventPacket *weightplotfeatureA, caerFrameEventPacket *synapseplotfeatureA, //used now
		caerFrameEventPacket *weightplotpoolingA, caerFrameEventPacket *synapseplotpoolingA,
		caerFrameEventPacket *weightplotfeatureB, caerFrameEventPacket *synapseplotfeatureB,
		caerFrameEventPacket *weightplotpoolingB, caerFrameEventPacket *synapseplotpoolingB,
		caerFrameEventPacket *weightplotoutputA, caerFrameEventPacket *synapseplotoutputA,
		caerFrameEventPacket *weightplotoutputB, caerFrameEventPacket *synapseplotoutputB) { //used now
	caerModuleData moduleData = caerMainloopFindModule(moduleID, "LFilter", CAER_MODULE_PROCESSOR);
	if (moduleData == NULL) {
		return;
	}
	caerModuleSM(&caerLearningFilterFunctions, moduleData, sizeof(struct LFilter_state), 14, eventSourceID, spike, //4
			weightplotfeatureA, synapseplotfeatureA, //used now
			weightplotpoolingA, synapseplotpoolingA,
			weightplotfeatureB, synapseplotfeatureB,
			weightplotpoolingB, synapseplotpoolingB,
			weightplotoutputA, synapseplotoutputA,
			weightplotoutputB, synapseplotoutputB); //used now
}

static bool caerLearningFilterInit(caerModuleData moduleData) {
	sshsNodePutIntIfAbsent(moduleData->moduleNode, "colorscaleMax", VMAX); //500
	sshsNodePutIntIfAbsent(moduleData->moduleNode, "colorscaleMin", VMIN);
	sshsNodePutByteIfAbsent(moduleData->moduleNode, "reset", 0);
	sshsNodePutFloatIfAbsent(moduleData->moduleNode, "resetExProbability", 1);
	sshsNodePutByteIfAbsent(moduleData->moduleNode, "resetExType", 1); //1 //2 for test, should be 1
	sshsNodePutByteIfAbsent(moduleData->moduleNode, "resetInType", 2); //1
	sshsNodePutDoubleIfAbsent(moduleData->moduleNode, "learningRateForward", 5); //1
	sshsNodePutDoubleIfAbsent(moduleData->moduleNode, "learningRateBackward", 2); //5 //2); //1
	sshsNodePutShortIfAbsent(moduleData->moduleNode, "dataSizeX", VISUALIZER_HEIGHT_FEATURE); //640
	sshsNodePutShortIfAbsent(moduleData->moduleNode, "dataSizeY", VISUALIZER_WIDTH_FEATURE); //480
	sshsNodePutShortIfAbsent(moduleData->moduleNode, "visualizerSizeX", VISUALIZER_HEIGHT_FEATURE); //640
	sshsNodePutShortIfAbsent(moduleData->moduleNode, "visualizerSizeY", VISUALIZER_WIDTH_FEATURE); //480
	sshsNodePutShortIfAbsent(moduleData->moduleNode, "apsSizeX", VISUALIZER_HEIGHT_FEATURE); //640
	sshsNodePutShortIfAbsent(moduleData->moduleNode, "apsSizeY", VISUALIZER_WIDTH_FEATURE); //480
	sshsNodePutBoolIfAbsent(moduleData->moduleNode, "stimulate", true); //false
	sshsNodePutBoolIfAbsent(moduleData->moduleNode, "learning", true); //false
	sshsNodePutIntIfAbsent(moduleData->moduleNode, "maxSynapseFeature", 3); //128); //500
	sshsNodePutIntIfAbsent(moduleData->moduleNode, "maxSynapseOutput", 127); //5 //128); //500

	LFilterState state = moduleData->moduleState;
	state->reset = sshsNodeGetByte(moduleData->moduleNode, "reset");
	state->resetExType = sshsNodeGetByte(moduleData->moduleNode, "resetExType");
	state->resetInType = sshsNodeGetByte(moduleData->moduleNode, "resetInType");
	state->dataSizeX = sshsNodeGetShort(moduleData->moduleNode, "dataSizeX");
	state->dataSizeY = sshsNodeGetShort(moduleData->moduleNode, "dataSizeY");
	state->visualizerSizeX = sshsNodeGetShort(moduleData->moduleNode, "visualizerSizeX");
	state->visualizerSizeY = sshsNodeGetShort(moduleData->moduleNode, "visualizerSizeY");
	state->apsSizeX = sshsNodeGetShort(moduleData->moduleNode, "apsSizeX");
	state->apsSizeY = sshsNodeGetShort(moduleData->moduleNode, "apsSizeY");
	state->stimulate = sshsNodeGetBool(moduleData->moduleNode, "stimulate");
	state->learning = sshsNodeGetBool(moduleData->moduleNode, "learning");
	state->maxSynapseFeature = sshsNodeGetInt(moduleData->moduleNode, "maxSynapseFeature");
	state->maxSynapseOutput = sshsNodeGetInt(moduleData->moduleNode, "maxSynapseOutput");

	sshsNodeAddAttributeListener(moduleData->moduleNode, moduleData, &caerModuleConfigDefaultListener);

	sshsNode sourceInfoNode = sshsGetRelativeNode(moduleData->moduleNode, "sourceInfo/");
	if (!sshsNodeAttributeExists(sourceInfoNode, "apsSizeX", SSHS_SHORT)) {
		sshsNodePutShort(sourceInfoNode, "apsSizeX", VISUALIZER_HEIGHT_FEATURE); //DYNAPSE_X4BOARD_NEUY
		sshsNodePutShort(sourceInfoNode, "apsSizeY", VISUALIZER_WIDTH_FEATURE); //DYNAPSE_X4BOARD_NEUY
	}

	return (true); // Nothing that can fail here.
}

static void caerLearningFilterConfig(caerModuleData moduleData) {
	caerModuleConfigUpdateReset(moduleData);
	LFilterState state = moduleData->moduleState;
	state->colorscaleMax = sshsNodeGetInt(moduleData->moduleNode, "colorscaleMax");
	state->colorscaleMin = sshsNodeGetInt(moduleData->moduleNode, "colorscaleMin");
	state->reset = sshsNodeGetByte(moduleData->moduleNode, "reset");
	state->resetExProbability = sshsNodeGetFloat(moduleData->moduleNode, "resetExProbability");
	state->resetExType = sshsNodeGetByte(moduleData->moduleNode, "resetExType");
	state->resetInType = sshsNodeGetByte(moduleData->moduleNode, "resetInType");
	state->learningRateForward = sshsNodeGetDouble(moduleData->moduleNode, "learningRateForward");
	state->learningRateBackward = sshsNodeGetDouble(moduleData->moduleNode, "learningRateBackward");
	state->dataSizeX = sshsNodeGetShort(moduleData->moduleNode, "dataSizeX");
	state->dataSizeY = sshsNodeGetShort(moduleData->moduleNode, "dataSizeY");
	state->visualizerSizeX = sshsNodeGetShort(moduleData->moduleNode, "visualizerSizeX");
	state->visualizerSizeY = sshsNodeGetShort(moduleData->moduleNode, "visualizerSizeY");
	state->visualizerSizeX = sshsNodeGetShort(moduleData->moduleNode, "apsSizeX");
	state->visualizerSizeY = sshsNodeGetShort(moduleData->moduleNode, "apsSizeY");
	state->stimulate = sshsNodeGetBool(moduleData->moduleNode, "stimulate");
	state->learning = sshsNodeGetBool(moduleData->moduleNode, "learning");
	state->maxSynapseFeature = sshsNodeGetInt(moduleData->moduleNode, "maxSynapseFeature");
	state->maxSynapseOutput = sshsNodeGetInt(moduleData->moduleNode, "maxSynapseOutput");
}

static void caerLearningFilterExit(caerModuleData moduleData) { // Remove listener, which can reference invalid memory in userData.
	sshsNodeRemoveAttributeListener(moduleData->moduleNode, moduleData, &caerModuleConfigDefaultListener);
}

static void caerLearningFilterReset(caerModuleData moduleData, uint16_t resetCallSourceID) {
	UNUSED_ARGUMENT(moduleData);
	UNUSED_ARGUMENT(resetCallSourceID);
//	ResetNetwork(moduleData);
}

static void caerLearningFilterRun(caerModuleData moduleData, size_t argsNumber, va_list args) {
	UNUSED_ARGUMENT(argsNumber);

	int16_t eventSourceID = (int16_t) va_arg(args, int); 	// Interpret variable arguments (same as above in main function).
	caerSpikeEventPacket spike = va_arg(args, caerSpikeEventPacket);
	caerFrameEventPacket *weightplotfeatureA = va_arg(args, caerFrameEventPacket*); //used now
	caerFrameEventPacket *synapseplotfeatureA = va_arg(args, caerFrameEventPacket*); //used now
//	caerFrameEventPacket *weightplotpoolingA = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *synapseplotpoolingA = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *weightplotfeatureB = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *synapseplotfeatureB = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *weightplotpoolingB = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *synapseplotpoolingB = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *weightplotoutputA = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *synapseplotoutputA = va_arg(args, caerFrameEventPacket*);
//	caerFrameEventPacket *weightplotoutputB = va_arg(args, caerFrameEventPacket*); //used now
//	caerFrameEventPacket *synapseplotoutputB = va_arg(args, caerFrameEventPacket*); //used now

	LFilterState state = moduleData->moduleState;
//	uint32_t counterW;
	uint32_t counterS;
//	COLOUR colW;
	COLOUR colS;
	uint16_t sizeX = VISUALIZER_HEIGHT_FEATURE;
	uint16_t sizeY = VISUALIZER_WIDTH_FEATURE;
	if (memory.synapseMap == NULL) {
		int64_t i, j, ys, row_id, col_id, feature_id;
		//initialize lookup tables
		for (i = 0; i < DELTA_WEIGHT_LUT_LENGTH; i++) {
			deltaWeights[i] = exp( (double) i/1000 );
		}
		for (i = 0; i < SYNAPSE_UPGRADE_THRESHOLD_LUT_LENGTH; i++) {
			synapseUpgradeThreshold[i] = exp( (double) i/1000 ); //i //i/2 i/100 i/10 too small //i 0 //1; //exp( (double) i/1000);
		}
		if (!ResetNetwork(moduleData, eventSourceID)) { // Failed to allocate memory, nothing to do.
			caerLog(CAER_LOG_ERROR, moduleData->moduleSubSystemString, "Failed to allocate memory for synapseMap.");
			return;
		}
/*		double warrayW[sizeX][sizeY];
		for (i = 0; i < sizeX; i++)
			for (j = 0; j < sizeY; j++)
				warrayW[i][j] = 0;
		*weightplotfeatureA = caerFrameEventPacketAllocate(1, I16T(moduleData->moduleID), 0, sizeX, sizeY, 3); //put info into frame
		if (*weightplotfeatureA != NULL) {
			caerFrameEvent singleplotW = caerFrameEventPacketGetEvent(*weightplotfeatureA, 0);
			//for feature maps
			for (i = 0; i < INPUT_N; i++) {
				for (j = 0; j < FEATURE1_N * FEATURE1_LAYERS_N; j++) {
					if ((int)(i/INPUT_L) >= (int)((j%FEATURE1_N)/FEATURE1_L)
							&& (int)(i/INPUT_L) < (int)((j%FEATURE1_N)/FEATURE1_L) + FILTER1_L
							&& i%INPUT_W >= (j%FEATURE1_N)%FEATURE1_W
							&& i%INPUT_W < (j%FEATURE1_N)%FEATURE1_W + FILTER1_W) {
						row_id = FILTER1_L*(int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
						col_id = FILTER1_W*((j%FEATURE1_N)%FEATURE1_W)+i%INPUT_W-(j%FEATURE1_N)%FEATURE1_W;
						feature_id = (int)(j/FEATURE1_N);
						warrayW[(feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id][(feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id]
							= memory.weightMap->buffer2d[((i & 0xf) | ((i & 0x10) >> 4) << 8 | ((i & 0x1e0) >> 5) << 4 | ((i & 0x200) >> 9) << 9)][j+TOTAL_NEURON_NUM_ON_CHIP*2]; //i
					}
				}
			}
			//for output layer
			for (i = 0; i < FEATURE1_N * FEATURE1_LAYERS_N; i++) {
				for (j = 0; j < OUTPUT2_N; j++) {
					row_id = (int) (i/16);
					col_id = i%16 + j * 16;
					warrayW[FILTER1_L*FEATURE1_L + row_id][FILTER1_W*FEATURE1_W + col_id]
							= memory.weightMap->buffer2d[i+TOTAL_NEURON_NUM_ON_CHIP*2][j+TOTAL_NEURON_NUM_ON_CHIP*2+TOTAL_NEURON_NUM_IN_CORE];
				}
			}
			counterW = 0;
			for (i = 0; i < sizeX; i++) {
				for (ys = 0; ys < sizeY; ys++) {
					colW  = GetColourW((double) warrayW[i][ys], (double) VMIN, (double) VMAX); //-500, 500); // warray[i][ys]/1000
					singleplotW->pixels[counterW] = colW.r; //(uint16_t) ( (int)(colW.r) ); // 65535		// red
					singleplotW->pixels[counterW + 1] = colW.g; //(uint16_t) ( (int)(colW.g) ); // 65535	// green
					singleplotW->pixels[counterW + 2] = colW.b; //(uint16_t) ( (int)(colW.b) ); // 65535	// blue
					counterW += 3;
				}
			}
			caerFrameEventSetLengthXLengthYChannelNumber(singleplotW, sizeX, sizeY, 3, *weightplotfeatureA); //add info to the frame
			caerFrameEventValidate(singleplotW, *weightplotfeatureA); //validate frame
		} */
		int warrayS[sizeX][sizeY];
		for (i = 0; i < sizeX; i++)
			for (j = 0; j < sizeY; j++)
				warrayS[i][j] = 0;
		*synapseplotfeatureA = caerFrameEventPacketAllocate(1, I16T(moduleData->moduleID), 0, sizeX, sizeY, 3); //put info into frame
		if (*synapseplotfeatureA != NULL) {
			caerFrameEvent singleplotS = caerFrameEventPacketGetEvent(*synapseplotfeatureA, 0);
			//for feature maps
			for (i = 0; i < INPUT_N; i++) {
				for (j = 0; j < FEATURE1_N * FEATURE1_LAYERS_N; j++) {
					if ((int)(i/INPUT_L) >= (int)((j%FEATURE1_N)/FEATURE1_L)
							&& (int)(i/INPUT_L) < (int)((j%FEATURE1_N)/FEATURE1_L) + FILTER1_L
							&& i%INPUT_W >= (j%FEATURE1_N)%FEATURE1_W
							&& i%INPUT_W < (j%FEATURE1_N)%FEATURE1_W + FILTER1_W) {
						row_id = FILTER1_L*(int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
						col_id = FILTER1_W*((j%FEATURE1_N)%FEATURE1_W) + i%INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
						feature_id = (int)(j/FEATURE1_N);
						warrayS[(feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id][(feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id]
							= memory.synapseMap->buffer2d[((i & 0xf) | ((i & 0x10) >> 4) << 8 | ((i & 0x1e0) >> 5) << 4 | ((i & 0x200) >> 9) << 9)][j+TOTAL_NEURON_NUM_ON_CHIP*2]; //i
					}
				}
			}
			//for output layer
			for (i = 0; i < FEATURE1_N * FEATURE1_LAYERS_N; i++) {
				for (j = 0; j < OUTPUT2_N; j++) {
					row_id = (int) (i/16);
					col_id = i%16 + j * 16;
					warrayS[FILTER1_L*FEATURE1_L + row_id][FILTER1_W*FEATURE1_W + col_id]
							= memory.synapseMap->buffer2d[i+TOTAL_NEURON_NUM_ON_CHIP*2][j+TOTAL_NEURON_NUM_ON_CHIP*2+TOTAL_NEURON_NUM_IN_CORE*3];
				}
			}
			counterS = 0;
			for (i = 0; i < sizeX; i++) {
				for (ys = 0; ys < sizeY; ys++) {
					colS  = GetColourS((int) warrayS[ys][i]); //warrayS[i][ys]); //, (double) VMIN, (double) VMAX //-500, 500); //
					singleplotS->pixels[counterS] = colS.r; //(uint16_t) ( (int)(colW.r) ); //*65535		// red
					singleplotS->pixels[counterS + 1] = colS.g; //(uint16_t) ( (int)(colW.g) ); //*65535	// green
					singleplotS->pixels[counterS + 2] = colS.b; //(uint16_t) ( (int)(colW.b) ); //*65535	// blue
					counterS += 3;
				}
			}
			caerFrameEventSetLengthXLengthYChannelNumber(singleplotS, sizeX, sizeY, 3, *synapseplotfeatureA); //add info to the frame
			caerFrameEventValidate(singleplotS, *synapseplotfeatureA); //validate frame
		}
	}

	if (spike == NULL) { // Only process packets with content.
		return;
	}

	caerLearningFilterConfig(moduleData); // Update parameters
	int64_t neuronAddr = 0;

	if (state->reset == 1) {
		if (reseted == 0) {
			ResetNetwork(moduleData, eventSourceID);
			printf("\nNetwork reseted \n");
			int64_t i, j, ys, row_id, col_id, feature_id;
/*			double warrayW[sizeX][sizeY];
			for (i = 0; i < sizeX; i++)
				for (j = 0; j < sizeY; j++)
					warrayW[i][j] = 0;
			*weightplotfeatureA = caerFrameEventPacketAllocate(1, I16T(moduleData->moduleID), 0, sizeX, sizeY, 3); //put info into frame
			if (*weightplotfeatureA != NULL) {
				caerFrameEvent singleplotW = caerFrameEventPacketGetEvent(*weightplotfeatureA, 0);
				for (i = 0; i < INPUT_N; i++) {
					for (j = 0; j < FEATURE1_N * FEATURE1_LAYERS_N; j++) {
						if ((int)(i/INPUT_L) >= (int)((j%FEATURE1_N)/FEATURE1_L)
								&& (int)(i/INPUT_L) < (int)((j%FEATURE1_N)/FEATURE1_L) + FILTER1_L
								&& i%INPUT_W >= (j%FEATURE1_N)%FEATURE1_W
								&& i%INPUT_W < (j%FEATURE1_N)%FEATURE1_W + FILTER1_W) {
							row_id = FILTER1_L*(int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							col_id = FILTER1_W*((j%FEATURE1_N)%FEATURE1_W)+i%INPUT_W-(j%FEATURE1_N)%FEATURE1_W;
							feature_id = (int)(j/FEATURE1_N);
							warrayW[(feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id][(feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id]
								= memory.weightMap->buffer2d[((i & 0xf) | ((i & 0x10) >> 4) << 8 | ((i & 0x1e0) >> 5) << 4 | ((i & 0x200) >> 9) << 9)][j+TOTAL_NEURON_NUM_ON_CHIP*2]; //i
						}
					}
				}
				counterW = 0;
				for (i = 0; i < sizeX; i++) {
					for (ys = 0; ys < sizeY; ys++) {
						colW  = GetColourW((double) warrayW[i][ys], (double) VMIN, (double) VMAX); //-500, 500); // warray[i][ys]/1000
						singleplotW->pixels[counterW] = colW.r; //(uint16_t) ( (int)(colW.r) ); // 65535		// red
						singleplotW->pixels[counterW + 1] = colW.g; //(uint16_t) ( (int)(colW.g) ); // 65535	// green
						singleplotW->pixels[counterW + 2] = colW.b; //(uint16_t) ( (int)(colW.b) ); // 65535	// blue
						counterW += 3;
					}
				}
				caerFrameEventSetLengthXLengthYChannelNumber(singleplotW, sizeX, sizeY, 3, *weightplotfeatureA); //add info to the frame
				caerFrameEventValidate(singleplotW, *weightplotfeatureA); //validate frame
			} */
			int warrayS[sizeX][sizeY];
			for (i = 0; i < sizeX; i++)
				for (j = 0; j < sizeY; j++)
					warrayS[i][j] = 0;
			*synapseplotfeatureA = caerFrameEventPacketAllocate(1, I16T(moduleData->moduleID), 0, sizeX, sizeY, 3); //put info into frame
			if (*synapseplotfeatureA != NULL) {
				caerFrameEvent singleplotS = caerFrameEventPacketGetEvent(*synapseplotfeatureA, 0);
				//for feature maps
				for (i = 0; i < INPUT_N; i++) {
					for (j = 0; j < FEATURE1_N * FEATURE1_LAYERS_N; j++) {
						if ((int)(i/INPUT_L) >= (int)((j%FEATURE1_N)/FEATURE1_L)
								&& (int)(i/INPUT_L) < (int)((j%FEATURE1_N)/FEATURE1_L) + FILTER1_L
								&& i%INPUT_W >= (j%FEATURE1_N)%FEATURE1_W
								&& i%INPUT_W < (j%FEATURE1_N)%FEATURE1_W + FILTER1_W) {
							row_id = FILTER1_L*(int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							col_id = FILTER1_W*((j%FEATURE1_N)%FEATURE1_W) + i%INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
							feature_id = (int)(j/FEATURE1_N);
							warrayS[(feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id][(feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id]
								= memory.synapseMap->buffer2d[((i & 0xf) | ((i & 0x10) >> 4) << 8 | ((i & 0x1e0) >> 5) << 4 | ((i & 0x200) >> 9) << 9)][j+TOTAL_NEURON_NUM_ON_CHIP*2]; //i
						}
					}
				}
				//for output layer
				for (i = 0; i < FEATURE1_N * FEATURE1_LAYERS_N; i++) {
					for (j = 0; j < OUTPUT2_N; j++) {
						row_id = (int) (i/16);
						col_id = i%16 + j * 16;
						warrayS[FILTER1_L*FEATURE1_L + row_id][FILTER1_W*FEATURE1_W + col_id]
								= memory.synapseMap->buffer2d[i+TOTAL_NEURON_NUM_ON_CHIP*2][j*6+TOTAL_NEURON_NUM_ON_CHIP*2+TOTAL_NEURON_NUM_IN_CORE*3];
					}
				}
				counterS = 0;
				for (i = 0; i < sizeX; i++) {
					for (ys = 0; ys < sizeY; ys++) {
						colS  = GetColourS((int) warrayS[ys][i]); //warrayS[i][ys]); //, (double) VMIN, (double) VMAX //-500, 500); //
						singleplotS->pixels[counterS] = colS.r; //(uint16_t) ( (int)(colW.r) ); //*65535		// red
						singleplotS->pixels[counterS + 1] = colS.g; //(uint16_t) ( (int)(colW.g) ); //*65535	// green
						singleplotS->pixels[counterS + 2] = colS.b; //(uint16_t) ( (int)(colW.b) ); //*65535	// blue
						counterS += 3;
					}
				}
			}
			printf("Visualizer reseted \n");
			reseted = 1;
		}
	} else {
		reseted = 0;
	}

	if (state->stimulate == true) { //run when there is a spike
/*		if (stimulating == 1 && abs(time_count - time_count_last) >= 5) {
			DisableStimuliGen(moduleData, eventSourceID); //EnableStimuliGen(moduleData, eventSourceID, 1); //DisableStimuliGen(moduleData, eventSourceID);
			time_count_last = time_count;
			stimulating = 0;
		}
		else if (stimulating == 0 && abs(time_count - time_count_last) >= 1) {
			stimuliPattern = (stimuliPattern + 1) % 3 + 4;
			EnableStimuliGen(moduleData, eventSourceID, stimuliPattern); //4 //stimuliPattern
			time_count_last = time_count;
			stimulating = 1;
		} */
		if (abs(time_count - time_count_last) >= 5) { //20) { //10) { //5 //1
			stimuliPattern = stimuliPattern % 3 + 7; //(stimuliPattern + 1) % 3 + 7; //4 for one2one source address; 7 for single source address
			DisableTeachingSignal(moduleData, eventSourceID);
			EnableStimuliGen(moduleData, eventSourceID, stimuliPattern); //4 //stimuliPattern
			teachingSignalDisabled = 1;
			time_count_last = time_count;
		}
		if (teachingSignalDisabled == 1 && abs(time_count - time_count_last) >= 1) { //1) { //2) { //1) { //2
			teachingSignalDisabled = 0;
			EnableTeachingSignal(moduleData, eventSourceID);
			time_count_last = time_count;
		}
	}
	else {
		DisableStimuliGen(moduleData, eventSourceID);
	}

	CAER_SPIKE_ITERATOR_VALID_START(spike) // Iterate over events and update weight

		if (state->learning == true) {
			EnableTeaching(moduleData, eventSourceID);

			int64_t ts = caerSpikeEventGetTimestamp64(caerSpikeIteratorElement, spike); // Get values on which to operate.

			uint32_t neuronId = caerSpikeEventGetNeuronID(caerSpikeIteratorElement);
			uint32_t coreId = caerSpikeEventGetSourceCoreID(caerSpikeIteratorElement);
			uint32_t chipId_t = caerSpikeEventGetChipID(caerSpikeIteratorElement);
			uint32_t chipId;

			if (chipId_t == 1) //DYNAPSE_CONFIG_DYNAPSE_U0 Why can I receive chip Id 1???
				chipId = 1;
			else if (chipId_t == DYNAPSE_CONFIG_DYNAPSE_U1)
				chipId = 2;
			else if (chipId_t == DYNAPSE_CONFIG_DYNAPSE_U2)
				chipId = 3;
			else if (chipId_t == DYNAPSE_CONFIG_DYNAPSE_U3)
				chipId = 4;

			if (chipId > 0 && chipId <= 4) {
				neuronAddr = chipId << 10 | coreId << 8 | neuronId;
				memory.spikeFifo->buffer2d[memory.wrPointer][0] = neuronAddr; // Put spike address into the queue
				memory.spikeFifo->buffer2d[memory.wrPointer][1] = ts; // Put spike address into the queue
				memory.spikeCounter += 1;
				memory.wrPointer = (memory.wrPointer + 1) % SPIKE_QUEUE_LENGTH;
			}

			uint8_t endSearching = 0;
			int64_t deltaTimeAccumulated = 0;

	//		int64_t i, j, row_id_t, col_id_t, row_id, col_id, feature_id;
			if (memory.wrPointer - memory.preRdPointer >= MINIMUM_CONSIDERED_SPIKE_NUM) {

				int64_t preSpikeAddr = memory.spikeFifo->buffer2d[memory.preRdPointer][0];
				int64_t preSpikeTime = memory.spikeFifo->buffer2d[memory.preRdPointer][1];
				memory.spikeCounter -= 1;
				memory.preRdPointer = (memory.preRdPointer + 1) % SPIKE_QUEUE_LENGTH;
				for (uint64_t postRdPointer = (memory.preRdPointer + 1) % SPIKE_QUEUE_LENGTH;
						endSearching != 1;
						postRdPointer = (postRdPointer + 1) % SPIKE_QUEUE_LENGTH) {
					int64_t postSpikeAddr = memory.spikeFifo->buffer2d[postRdPointer][0];
					int64_t postSpikeTime = memory.spikeFifo->buffer2d[postRdPointer][1];
					int64_t deltaTime = (int64_t) (postSpikeTime - preSpikeTime); //should be positive

					if (deltaTime <= 0)
						break;

					if (deltaTime < MAXIMUM_CONSIDERED_SPIKE_DELAY) {
						double deltaWeight = deltaWeights[DELTA_WEIGHT_LUT_LENGTH-deltaTime];
						if (memory.connectionMap->buffer2d[preSpikeAddr-MEMORY_NEURON_ADDR_OFFSET][postSpikeAddr-MEMORY_NEURON_ADDR_OFFSET] == 1) {
							ModifyForwardSynapse(moduleData, eventSourceID, preSpikeAddr, postSpikeAddr, deltaWeight, synapseplotfeatureA, weightplotfeatureA);
						}
						//there was a mistake, pre-post pair changed
						if (memory.connectionMap->buffer2d[postSpikeAddr-MEMORY_NEURON_ADDR_OFFSET][preSpikeAddr-MEMORY_NEURON_ADDR_OFFSET] == 1) {
							ModifyBackwardSynapse(moduleData, preSpikeAddr, postSpikeAddr, deltaWeight, weightplotfeatureA); //, eventSourceID
						}
					}

					if (postRdPointer == memory.wrPointer - 1)
						endSearching = 1;
					else if (deltaTimeAccumulated > MAXIMUM_CONSIDERED_SPIKE_DELAY)
						endSearching = 1;
					deltaTimeAccumulated += deltaTime;
				}
			}
		} else {
			DisableTeaching(moduleData, eventSourceID);
		}

	CAER_SPIKE_ITERATOR_VALID_END

}

void ModifyForwardSynapse(caerModuleData moduleData, int16_t eventSourceID, int64_t preSpikeAddr, int64_t postSpikeAddr, double deltaWeight,
		caerFrameEventPacket *synapseplotfeatureA, caerFrameEventPacket *weightplotfeatureA) {

	LFilterState state = moduleData->moduleState;

	double new_weight;
	int64_t min = MEMORY_NEURON_ADDR_OFFSET;
	int64_t i, j, preAddr, postAddr;
//	int32_t filterSize;
	int64_t	preNeuronId;
	uint32_t camId;
	int8_t synapseType = 0;
	int8_t synapseUpgrade = 0;
	int64_t preNeuronAddr, postNeuronAddr;
	int32_t new_synapse_add = 0;
	int32_t new_synapse_sub = 0;
	int32_t replaced_synapse = 0;
	preNeuronAddr = preSpikeAddr; postNeuronAddr = postSpikeAddr;

	uint32_t counterS;
	COLOUR colS;
	int64_t row_id_t, col_id_t, row_id, col_id, feature_id;
	caerFrameEvent singleplotS;

	int32_t output_disabled = 0;
	int output_counter = 0;
	int64_t neuron_address;
	uint32_t post_neuron_address;

	uint32_t k;

	if (memory.connectionMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] == 1) {

		new_weight = memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] + deltaWeight * state->learningRateForward;
//		filterSize = memory.filterMapSize->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][0];
//		int8_t camFound = 0;
		double increased_weight = 0;
		if (new_weight > memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET]) {

			int current_synapse = memory.synapseMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET];
			synapseUpgrade = 0;

/*			output_found = 0;
			if ((postNeuronAddr & 0x3c00) == 3 && (postNeuronAddr & 0x300) == 3) {
				for (i = 0; i < 3; i++) {
					if (memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i] == 1) {
						output_found = 1;
						break;
					}
				}
				if (output_found == 1 && (postNeuronAddr & 0xff) != i) {
					if (new_weight > memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(3 << 10 | 3 << 8 | i) - MEMORY_NEURON_ADDR_OFFSET]) {
						memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr & 0xff] = 1;
						memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i] = 0;
						for (j = 0; j < TOTAL_CAM_NUM_LEARNING; j++) {
							neuron_address = memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][j];
							if (neuron_address == preNeuronAddr) {
								WriteCam(moduleData, eventSourceID, 0, (uint32_t) postNeuronAddr, 0, (uint32_t) j, 0, 0);
								// update other related information
							}
						}
					} else {
						memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_weight;
					}
				} else if (output_found == 0) {
					memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr & 0xff] = 1;
				}
			} */

/*			output_disabled = 0;
//			output_counter = 0;
			if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
				for (i = 0; i < 3; i++) {
					if (memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i] == 1 && (postNeuronAddr & 0xff) != i) {
						output_disabled = 1;
						break;
//						output_counter += 1;
					}
				}
			} */

			output_disabled = 0;
			if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
				output_disabled = memory.outputMapDisabled->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)/6]; //0; //
				if (output_disabled == 0) {
					output_counter = 0;
					for (i = 0; i < 3; i++) {
						if (memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i] == 1) {
							output_counter += 1;
						}
					}
					if (output_counter >= 2 || (output_counter == 1 && memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)/6] != 1)) {
						output_disabled = 1;
	//					memory.outputMapDisabled->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)] = 1;
						for (k = 0; k < 3; k++) {
							post_neuron_address = 3 << 10 | 3 << 8 | (k*6);
							for (i = 0; i < 3; i++) {
								if (memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i] == 1) {
									for (j = 0; j < 61; j++) {
										camId = (uint32_t) j;
										if (memory.camMap->buffer2d[post_neuron_address-MEMORY_NEURON_ADDR_OFFSET][camId] != 0) {
											neuron_address = memory.camMapContentSource->buffer2d[post_neuron_address-MEMORY_NEURON_ADDR_OFFSET][camId];
											if (neuron_address == preNeuronAddr) {
												WriteCam(moduleData, eventSourceID, 0, post_neuron_address, 0, (uint32_t) camId, 0, 0);
												memory.camMap->buffer2d[post_neuron_address-MEMORY_NEURON_ADDR_OFFSET][camId] = 0;
												memory.camSize->buffer2d[post_neuron_address-MEMORY_NEURON_ADDR_OFFSET][0] -= 1;
												memory.connectionCamMap->buffer2d[post_neuron_address-MEMORY_NEURON_ADDR_OFFSET][preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = 0;
												memory.camMapContentType->buffer2d[post_neuron_address-MEMORY_NEURON_ADDR_OFFSET][camId] = 0;
												memory.camMapContentSource->buffer2d[post_neuron_address-MEMORY_NEURON_ADDR_OFFSET][camId] = 0;
											}
										}
									}
								}
							}
							memory.outputMapDisabled->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][k] = 1;
							memory.synapseMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][post_neuron_address-MEMORY_NEURON_ADDR_OFFSET] = 0;
							if (*synapseplotfeatureA != NULL) {
								singleplotS = caerFrameEventPacketGetEvent(*synapseplotfeatureA, 0);
							}
							if (*synapseplotfeatureA != NULL) {
								int64_t preNeuronAddr_t = preNeuronAddr;
								int64_t postNeuronAddr_t = post_neuron_address;
								colS  = GetColourS(new_synapse_sub); //, (double) VMIN, (double) VMAX //4 //0
								preAddr = (preNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
								postAddr = (postNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
								i = preAddr; //(preAddr & 0xf) | ((preAddr & 0x100) >> 8) << 4 | ((preAddr & 0xf0) >> 4) << 5 | ((preAddr & 0x200) >> 9) << 9;
								j = postAddr;
	//							feature_id = (int)(j/FEATURE1_N);
								row_id_t = (int) (i/16);
								col_id_t = i%16 + (j%3) * 16;
	//							row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
								row_id = FILTER1_L*FEATURE1_L + row_id_t;
	//							col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
								col_id = FILTER1_W*FEATURE1_W + col_id_t;
								counterS = (uint32_t) ((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3; //((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
								singleplotS->pixels[counterS] = colS.r;
								singleplotS->pixels[counterS + 1] = colS.g;
								singleplotS->pixels[counterS + 2] = colS.b;
							}
						}

					}
				}
			}

			if (deltaWeight * state->learningRateForward > synapseUpgradeThreshold[current_synapse] &&
					(
						((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 != 3 && current_synapse <= state->maxSynapseFeature) ||
						((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 == 3 && current_synapse <= state->maxSynapseOutput && output_disabled == 0) //&& output_disabled == 0
					)
				) { // <= 5 //128
				increased_weight = increased_weight + deltaWeight * state->learningRateForward;

				int slowFound = 0;
				int minFound = 0;
				double current_weight = 0;
				double current_weight_t = 0;
				double min_weight = 0;
				int synapseType_t = 0;

				int availableCamFound = 0;
				uint32_t cam_id;

				int camsize = 0;
				uint32_t camsize_limit = 0;

				if ((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 != 3) {
					camsize_limit = TOTAL_CAM_NUM_LEARNING;
				} else if ((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 == 3) {
					camsize_limit = 60;
				}

				camsize = memory.camSize->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][0];

				if (camsize < (int) camsize_limit) {
					for(cam_id = 0; cam_id < camsize_limit; cam_id++) { //search for available CAM
						if (memory.camMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][cam_id] == 0){
							camId = cam_id;
							availableCamFound = 1;
							break;
						}
					}
				}

				if (availableCamFound == 0) {
					for (i = 0; i < camsize_limit; i++) {
						if (memory.camMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i] != 0) {
							preNeuronId = memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i];
							synapseType_t = memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i];
							if (synapseType_t > 0) { //Real synapse exists
								if (preNeuronId != preNeuronAddr && minFound == 0) {
									minFound = 1;
									min = preNeuronId;
									min_weight = memory.weightMap->buffer2d[min-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET];
									camId = (uint32_t) i;
									replaced_synapse = memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i];
								}
								if (preNeuronId == preNeuronAddr && synapseType_t == EXCITATORY_SLOW_SYNAPSE_ID) { //synapse already exists
									slowFound = 1;
									camId = (uint32_t) i;
									break;
								}
								current_weight_t = memory.weightMap->buffer2d[preNeuronId-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET];
								if (minFound == 1 && preNeuronId != preNeuronAddr && current_weight_t < min_weight) {
									min = preNeuronId;
									min_weight = memory.weightMap->buffer2d[min-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET];
									camId = (uint32_t) i;
									replaced_synapse = memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i];
								}
							}
						}
					}
				}

				if (availableCamFound == 1) {
					current_weight = memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET];
					new_weight = current_weight + increased_weight;
					synapseUpgrade = 1;
					synapseType = EXCITATORY_SLOW_SYNAPSE_ID;
					DisableStimuliGenPrimitiveCam(moduleData, eventSourceID);
					WriteCam(moduleData, eventSourceID, (uint32_t) preNeuronAddr, (uint32_t) postNeuronAddr, 0, camId, synapseType, 0);
					EnableStimuliGenPrimitiveCam(moduleData, eventSourceID);
					if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
						memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)/6] = 1;
					}
					new_synapse_add = current_synapse + EXCITATORY_SLOW_SYNAPSE_ID;
					memory.synapseMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_synapse_add;
					memory.connectionCamMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = (int32_t) camId;
					memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = EXCITATORY_SLOW_SYNAPSE_ID;
					memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = (int32_t) preNeuronAddr;

					memory.camMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = (int32_t) preNeuronAddr;
					memory.camSize->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][0] += 1;
				}

				if (slowFound == 1) {
					synapseUpgrade = 1;
					synapseType = EXCITATORY_FAST_SYNAPSE_ID;
					DisableStimuliGenPrimitiveCam(moduleData, eventSourceID);
//					DisableStimuliGen(moduleData, eventSourceID);
					WriteCam(moduleData, eventSourceID, (uint32_t) preNeuronAddr, (uint32_t) postNeuronAddr, 0, camId, synapseType, 0);
					EnableStimuliGenPrimitiveCam(moduleData, eventSourceID);
					if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
						memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)/6] = 1;
					}
//					EnableStimuliGen(moduleData, eventSourceID, stimuliPattern);
					new_synapse_add = current_synapse + (EXCITATORY_FAST_SYNAPSE_ID - EXCITATORY_SLOW_SYNAPSE_ID);
					memory.synapseMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_synapse_add;
					memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = EXCITATORY_FAST_SYNAPSE_ID;
					memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_weight;
					//update weight plot
/*					if (*weightplotfeatureA != NULL) {
//						int64_t i, j, row_id, row_id_t, col_id, col_id_t, feature_id;
						uint32_t counterW;
						COLOUR colW;
						caerFrameEvent singleplotW = caerFrameEventPacketGetEvent(*weightplotfeatureA, 0);
						if (memory.connectionMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] == 1) {
			//				double new_weight = memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] + deltaWeight * state->learningRate;
							colW  = GetColourW(new_weight, (double) VMIN, (double) VMAX);
							i = preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET;
							j = (postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							feature_id = (int)(j/FEATURE1_N);
							row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							row_id = (feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id_t;
							col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
							col_id = (feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id_t;
							counterW = (uint32_t) ((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
							singleplotW->pixels[counterW] = colW.r;
							singleplotW->pixels[counterW + 1] = colW.g;
							singleplotW->pixels[counterW + 2] = colW.b;
						}
					} */
				} else if (minFound == 1 && increased_weight > min_weight) {
					current_weight = memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET];
					new_weight = current_weight + increased_weight;
					synapseUpgrade = 1;
					synapseType = EXCITATORY_SLOW_SYNAPSE_ID;
//					camId = (uint32_t) memory.connectionCamMap->buffer2d[min-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET]; //replace the CAM of MIN by the strengthened one
					DisableStimuliGenPrimitiveCam(moduleData, eventSourceID);
//					DisableStimuliGen(moduleData, eventSourceID);
					WriteCam(moduleData, eventSourceID, (uint32_t) preNeuronAddr, (uint32_t) postNeuronAddr, 0, camId, synapseType, 0);
					EnableStimuliGenPrimitiveCam(moduleData, eventSourceID);
					if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
						memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)/6] = 1;
					}
//					EnableStimuliGen(moduleData, eventSourceID, stimuliPattern);
					new_synapse_add = current_synapse + EXCITATORY_SLOW_SYNAPSE_ID;
					new_synapse_sub = memory.synapseMap->buffer2d[min-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] - replaced_synapse;
					if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
						if (new_synapse_sub == 0) {
							memory.outputMap->buffer2d[min-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)/6] = 0;
						}
					}
					memory.synapseMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_synapse_add;
					memory.synapseMap->buffer2d[min-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_synapse_sub; //NO_SYNAPSE_ID
					memory.connectionCamMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][min-MEMORY_NEURON_ADDR_OFFSET] = 0;
					memory.connectionCamMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = (int32_t) camId;
					memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = EXCITATORY_SLOW_SYNAPSE_ID;
					memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = (int32_t) preNeuronAddr;
					if ((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 != 3) {
						if (*synapseplotfeatureA != NULL) {
							singleplotS = caerFrameEventPacketGetEvent(*synapseplotfeatureA, 0);
						}
						if (*synapseplotfeatureA != NULL) {
							int64_t preNeuronAddr_t = min;
							int64_t postNeuronAddr_t = postSpikeAddr;
							colS  = GetColourS(new_synapse_sub); //, (double) VMIN, (double) VMAX //4 //0
							preAddr = preNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET;
							postAddr = (postNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							i = (preAddr & 0xf) | ((preAddr & 0x100) >> 8) << 4 | ((preAddr & 0xf0) >> 4) << 5 | ((preAddr & 0x200) >> 9) << 9;
							j = postAddr;
							feature_id = (int)(j/FEATURE1_N);
							row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							row_id = (feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id_t;
							col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
							col_id = (feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id_t;
							counterS = (uint32_t) ((col_id * VISUALIZER_WIDTH_FEATURE) + row_id) * 3; //((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
							singleplotS->pixels[counterS] = colS.r;
							singleplotS->pixels[counterS + 1] = colS.g;
							singleplotS->pixels[counterS + 2] = colS.b;
						}
					} else if ((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 == 3) {
						if (*synapseplotfeatureA != NULL) {
							singleplotS = caerFrameEventPacketGetEvent(*synapseplotfeatureA, 0);
						}
						if (*synapseplotfeatureA != NULL) {
							int64_t preNeuronAddr_t = min;
							int64_t postNeuronAddr_t = postSpikeAddr;
							colS  = GetColourS(new_synapse_sub); //, (double) VMIN, (double) VMAX //4 //0
							preAddr = (preNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							postAddr = (postNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							i = preAddr; //(preAddr & 0xf) | ((preAddr & 0x100) >> 8) << 4 | ((preAddr & 0xf0) >> 4) << 5 | ((preAddr & 0x200) >> 9) << 9;
							j = postAddr;
//							feature_id = (int)(j/FEATURE1_N);
							row_id_t = (int) (i/16);
							col_id_t = i%16 + ((j%256)/6) * 16;
//							row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							row_id = FILTER1_L*FEATURE1_L + row_id_t;
//							col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
							col_id = FILTER1_W*FEATURE1_W + col_id_t;
							counterS = (uint32_t) ((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3; //((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
							singleplotS->pixels[counterS] = colS.r;
							singleplotS->pixels[counterS + 1] = colS.g;
							singleplotS->pixels[counterS + 2] = colS.b;
						}
					}

					memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_weight;
//					memory.weightMap->buffer2d[min-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = 0;
					//update weight plot
/*					if (*weightplotfeatureA != NULL) {
//						int64_t i, j, row_id, row_id_t, col_id, col_id_t, feature_id;
						uint32_t counterW;
						COLOUR colW;
						caerFrameEvent singleplotW = caerFrameEventPacketGetEvent(*weightplotfeatureA, 0);
						if (memory.connectionMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] == 1) {
			//				double new_weight = memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] + deltaWeight * state->learningRate;
							colW  = GetColourW(new_weight, (double) VMIN, (double) VMAX);
							i = preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET;
							j = (postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							feature_id = (int)(j/FEATURE1_N);
							row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							row_id = (feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id_t;
							col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
							col_id = (feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id_t;
							counterW = (uint32_t) ((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
							singleplotW->pixels[counterW] = colW.r;
							singleplotW->pixels[counterW + 1] = colW.g;
							singleplotW->pixels[counterW + 2] = colW.b;
						}
					} */
				}

				if (synapseUpgrade == 1) {
					if ((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 != 3) {
						if (*synapseplotfeatureA != NULL) {
							singleplotS = caerFrameEventPacketGetEvent(*synapseplotfeatureA, 0);
						}
						if (*synapseplotfeatureA != NULL) {
							int64_t preNeuronAddr_t = preSpikeAddr;
							int64_t postNeuronAddr_t = postSpikeAddr;
							colS  = GetColourS(new_synapse_add); //, (double) VMIN, (double) VMAX
							preAddr = preNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET;
							postAddr = (postNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							i = (preAddr & 0xf) | ((preAddr & 0x100) >> 8) << 4 | ((preAddr & 0xf0) >> 4) << 5 | ((preAddr & 0x200) >> 9) << 9;
							j = postAddr;
							feature_id = (int)(j/FEATURE1_N);
							row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							row_id = (feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id_t;
							col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
							col_id = (feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id_t;
							counterS = (uint32_t) ((col_id * VISUALIZER_WIDTH_FEATURE) + row_id) * 3; //((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
							singleplotS->pixels[counterS] = colS.r;
							singleplotS->pixels[counterS + 1] = colS.g;
							singleplotS->pixels[counterS + 2] = colS.b;
						}
					} else if ((postSpikeAddr & 0x3c00) >> 10 == 3 && (postSpikeAddr & 0x300) >> 8 == 3) {
						if (*synapseplotfeatureA != NULL) {
							singleplotS = caerFrameEventPacketGetEvent(*synapseplotfeatureA, 0);
						}
						if (*synapseplotfeatureA != NULL) {
							int64_t preNeuronAddr_t = preSpikeAddr;
							int64_t postNeuronAddr_t = postSpikeAddr;
							colS  = GetColourS(new_synapse_add); //, (double) VMIN, (double) VMAX
							preAddr = (preNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							postAddr = (postNeuronAddr_t-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
							i = preAddr; //(preAddr & 0xf) | ((preAddr & 0x100) >> 8) << 4 | ((preAddr & 0xf0) >> 4) << 5 | ((preAddr & 0x200) >> 9) << 9;
							j = postAddr;
//							feature_id = (int)(j/FEATURE1_N);
							row_id_t = (int) (i/16);
							col_id_t = i%16 + ((j%256)/6) * 16;
//							row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
							row_id = FILTER1_L*FEATURE1_L + row_id_t;
//							col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
							col_id = FILTER1_W*FEATURE1_W + col_id_t;
							counterS = (uint32_t) ((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3; //((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
							singleplotS->pixels[counterS] = colS.r;
							singleplotS->pixels[counterS + 1] = colS.g;
							singleplotS->pixels[counterS + 2] = colS.b;
						}
					}
				}
			}
		}
	}
}

void ModifyBackwardSynapse(caerModuleData moduleData, int64_t preSpikeAddr, int64_t postSpikeAddr, double deltaWeight, caerFrameEventPacket *weightplotfeatureA) {

	LFilterState state = moduleData->moduleState;

	double new_weight;
	int64_t preNeuronAddr, postNeuronAddr;
	preNeuronAddr = postSpikeAddr; postNeuronAddr = preSpikeAddr;
	if (memory.connectionMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] == 1) {
		new_weight = memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] - deltaWeight * state->learningRateBackward;
		memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = new_weight;
		//update weight plot
/*		if (*weightplotfeatureA != NULL) {
			int64_t i, j, row_id, row_id_t, col_id, col_id_t, feature_id;
			uint32_t counterW;
			COLOUR colW;
			caerFrameEvent singleplotW = caerFrameEventPacketGetEvent(*weightplotfeatureA, 0);
			if (memory.connectionMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] == 1) {
//				double new_weight = memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] + deltaWeight * state->learningRate;
				colW  = GetColourW(new_weight, (double) VMIN, (double) VMAX);
				i = preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET;
				j = (postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET)%TOTAL_NEURON_NUM_ON_CHIP;
				feature_id = (int)(j/FEATURE1_N);
				row_id_t = FILTER1_L * (int)((j%FEATURE1_N)/FEATURE1_W) + (int)(i/INPUT_W) - (int)((j%FEATURE1_N)/FEATURE1_W);
				row_id = (feature_id >> 1)*FILTER1_L*FEATURE1_L + row_id_t;
				col_id_t = FILTER1_W * ((j%FEATURE1_N)%FEATURE1_W) + i % INPUT_W - (j%FEATURE1_N)%FEATURE1_W;
				col_id = (feature_id & 0x1)*FILTER1_W*FEATURE1_W + col_id_t;
				counterW = (uint32_t) ((row_id * VISUALIZER_WIDTH_FEATURE) + col_id) * 3;
				singleplotW->pixels[counterW] = colW.r;
				singleplotW->pixels[counterW + 1] = colW.g;
				singleplotW->pixels[counterW + 2] = colW.b;
			}
		} */
	}
}

//reset the network to the initial state
bool ResetNetwork(caerModuleData moduleData, int16_t eventSourceID)
{
	DisableStimuliGen(moduleData, eventSourceID);
	ResetBiases(moduleData, eventSourceID);
	time_count = 0;
	signal(SIGALRM, SignalHandler); //register the hand-made timer function
	SetTimer();

	ClearAllCam(moduleData, eventSourceID); //only for 1st chip
//	SetInputLayerCam(moduleData, eventSourceID);

	LFilterState state = moduleData->moduleState;
	int8_t exType = state->resetExType; //initial synapse type fast or slow
	int8_t inType = state->resetInType; //initial synapse type fast or slow

	memory.connectionMap = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_NEURON_NUM_ON_BOARD);
	memory.filterMap = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) MAXIMUM_FILTER_SIZE);
	memory.outputMap = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) OUTPUT2_N);
	memory.outputMapDisabled = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) OUTPUT2_N);
	memory.camMapContentSource = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_CAM_NUM);
	memory.camMapContentType = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_CAM_NUM);
	memory.connectionCamMap = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) MAXIMUM_FILTER_SIZE);
	memory.filterMapSize = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) FILTER_MAP_SIZE_WIDTH);

	memory.weightMap = simple2DBufferInitDouble((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_NEURON_NUM_ON_BOARD);
	memory.synapseMap = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_NEURON_NUM_ON_BOARD);
	memory.camMap = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_CAM_NUM);
	memory.camSize = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) CAM_SIZE_WIDTH);
	memory.sramMap = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_SRAM_NUM);
	memory.sramMapContent = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_SRAM_NUM);
	memory.spikeFifo = simple2DBufferInitLong((size_t) SPIKE_QUEUE_LENGTH, (size_t) SPIKE_QUEUE_WIDTH);

//	memory.inhibitoryValid = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_NEURON_NUM_ON_BOARD);
//	memory.inhibitoryVirtualNeuronAddr = simple2DBufferInitInt((size_t) TOTAL_NEURON_NUM_ON_BOARD, (size_t) TOTAL_NEURON_NUM_ON_BOARD);

	uint32_t chipId;
	uint32_t coreId;
	//create stimuli layer
	uint32_t neuronId;
/*	uint32_t stimuli_layer[INPUT_N];
	for (neuronId = 0; neuronId < INPUT_N; neuronId++) {
		chipId = VIRTUAL_CHIP_ID;
		stimuli_layer[neuronId] = chipId << NEURON_CHIPID_SHIFT |
				((neuronId & 0xf) | ((neuronId & 0x10) >> 4) << 8 | ((neuronId & 0x1e0) >> 5) << 4 | ((neuronId & 0x200) >> 9) << 9);
	}*/
	//create input layer
	uint32_t input_layer[INPUT_N];
	for (neuronId = 0; neuronId < INPUT_N; neuronId++) {
		chipId = CHIP_UP_LEFT_ID;
		input_layer[neuronId] = chipId << NEURON_CHIPID_SHIFT |
				((neuronId & 0xf) | ((neuronId & 0x10) >> 4) << 8 | ((neuronId & 0x1e0) >> 5) << 4 | ((neuronId & 0x200) >> 9) << 9);
	}
	//create feature layer 1
	uint32_t feature_layer1[FEATURE1_N * FEATURE1_LAYERS_N];
	for (neuronId = 0; neuronId < FEATURE1_N * FEATURE1_LAYERS_N; neuronId++) {
		chipId = CHIP_DOWN_LEFT_ID;
		feature_layer1[neuronId] = chipId << NEURON_CHIPID_SHIFT | neuronId;
	}
	//create output layer 2
	uint32_t output_layer2[OUTPUT2_N];
	for (neuronId = 0; neuronId < OUTPUT2_N; neuronId++) {
		chipId = CHIP_DOWN_LEFT_ID;
		coreId = CORE_DOWN_RIGHT_ID;
		if (neuronId == 0)
			output_layer2[neuronId] = chipId << NEURON_CHIPID_SHIFT | coreId << NEURON_COREID_SHIFT | 0;
		if (neuronId == 1)
			output_layer2[neuronId] = chipId << NEURON_CHIPID_SHIFT | coreId << NEURON_COREID_SHIFT | 6;
		if (neuronId == 2)
			output_layer2[neuronId] = chipId << NEURON_CHIPID_SHIFT | coreId << NEURON_COREID_SHIFT | 12;
	}
	//create pooling layer 1
/*	uint32_t pooling_layer1[POOLING1_N * POOLING1_LAYERS_N];
	for (neuronId = 0; neuronId < POOLING1_N * POOLING1_LAYERS_N; neuronId++) {
		chipId = CHIP_DOWN_LEFT_ID;
		coreId = CORE_DOWN_RIGHT_ID;
		pooling_layer1[neuronId] = chipId << NEURON_CHIPID_SHIFT | coreId << NEURON_COREID_SHIFT |
				(neuronId & 0x7) | ((neuronId & 0xf0) >> 4) << 3 | ((neuronId & 0x8) >> 3) << 7;
	} */
	//create feature layer 2
/*	uint32_t feature_layer2[FEATURE2_N * FEATURE2_LAYERS_N];
	for (neuronId = 0; neuronId < FEATURE2_N * FEATURE2_LAYERS_N; neuronId++) {
		chipId = CHIP_UP_RIGHT_ID;
		feature_layer2[neuronId] = chipId << NEURON_CHIPID_SHIFT |
				(neuronId & 0x3) | ((neuronId & 0xf0) >> 4) << 2 | ((neuronId & 0xc) >> 2) << 6 | ((neuronId & 0x300) >> 8) << 8;
	}
	//create pooling layer 2
	uint32_t pooling_layer2[POOLING2_N * POOLING2_LAYERS_N];
	for (neuronId = 0; neuronId < POOLING2_N * POOLING2_LAYERS_N; neuronId++) {
		chipId = CHIP_UP_RIGHT_ID;
		coreId = CORE_DOWN_LEFT_ID;
		pooling_layer2[neuronId] = chipId << NEURON_CHIPID_SHIFT | coreId << NEURON_COREID_SHIFT |
				(neuronId & 0x1) | ((neuronId & 0xf0) >> 4) << 2 | ((neuronId & 0xe) >> 1) << 5;
	}
	//create output layer 1
	uint32_t output_layer1[OUTPUT1_N];
	for (neuronId = 0; neuronId < OUTPUT1_N; neuronId++) {
		chipId = CHIP_DOWN_RIGHT_ID;
		output_layer1[neuronId] = chipId << NEURON_CHIPID_SHIFT | neuronId;
	}
	//create output layer 2
	uint32_t output_layer2[OUTPUT2_N];
	for (neuronId = 0; neuronId < OUTPUT2_N; neuronId++) {
		chipId = CHIP_DOWN_RIGHT_ID;
		coreId = CORE_DOWN_LEFT_ID;
		output_layer2[neuronId] = chipId << NEURON_CHIPID_SHIFT | coreId << NEURON_COREID_SHIFT | neuronId;
	} */

	int preNeuronId, postNeuronId;
	int core_id;
	uint32_t preNeuronAddr, postNeuronAddr;
	int randNumCount;
	uint32_t virtualNeuronAddr = 0;
	int8_t virtualNeuronAddrEnable = 0;

	int8_t inhibitoryValid[FEATURE1_LAYERS_N][TOTAL_NEURON_NUM_ON_CHIP];
	uint32_t inhibitoryVirtualNeuronCoreId[FEATURE1_LAYERS_N][TOTAL_NEURON_NUM_IN_CORE];

	for (core_id = 0; core_id < FEATURE1_LAYERS_N; core_id++) {
		for (neuronId = 0; neuronId < TOTAL_NEURON_NUM_ON_CHIP; neuronId++) {
			inhibitoryValid[core_id][neuronId] = 0;
		}
	}

	//randomly select 1 neuron from 4 neurons
	for (core_id = 0; core_id < FEATURE1_LAYERS_N; core_id++) {
		for (neuronId = 0; neuronId < TOTAL_NEURON_NUM_IN_CORE; neuronId++) {
			coreId = (uint32_t) (rand() % 4); //0 //randomly choose one value in 0, 1, 2, 3
			preNeuronAddr = coreId << NEURON_COREID_SHIFT | neuronId;
			inhibitoryValid[core_id][preNeuronAddr] = 1;
			inhibitoryVirtualNeuronCoreId[core_id][neuronId] = coreId;
		}
	}

	//stimuli to input
/*	for (preNeuronId = 0; preNeuronId < INPUT_N; preNeuronId++)
		for (postNeuronId = 0; postNeuronId < INPUT_N; postNeuronId++) {
			if (preNeuronId == postNeuronId) {
				BuildSynapse(moduleData, eventSourceID, stimuli_layer[preNeuronId], input_layer[postNeuronId], 2, EXTERNAL_REAL_SYNAPSE); //exType
			}
		} */
	//input to feature1
	for (postNeuronId = 0; postNeuronId < FEATURE1_N*FEATURE1_LAYERS_N; postNeuronId++) { //FEATURE1_N*FEATURE1_LAYERS_N //first sweep POST, then PRE
		//generate random binary number 1D array
		int64_t rand1DBinaryArray[FILTER1_N]; //FILTER1_N-FEATURE1_CAM_INHIBITORY_N
		GetRand1DBinaryArray(rand1DBinaryArray, FILTER1_N, TOTAL_CAM_NUM_LEARNING); //FILTER1_N-FEATURE1_CAM_INHIBITORY_N
		randNumCount = 0;
		for (preNeuronId = 0; preNeuronId < INPUT_N; preNeuronId++) {
			int pre_id = preNeuronId;
			int post_id = postNeuronId;
			if ((int)(pre_id/INPUT_W) >= (int)((post_id%FEATURE1_N)/FEATURE1_W)
					&& (int)(pre_id/INPUT_W) < (int)((post_id%FEATURE1_N)/FEATURE1_W) + FILTER1_L
					&& pre_id%INPUT_W >= (post_id%FEATURE1_N)%FEATURE1_W
					&& pre_id%INPUT_W < (post_id%FEATURE1_N)%FEATURE1_W + FILTER1_W) {
				//randomly reset, depends on the ratio of total CAM number and FILTER1_N-FEATURE1_CAM_INHIBITORY_N
				preNeuronAddr = input_layer[preNeuronId];
				postNeuronAddr = feature_layer1[postNeuronId];
				if (rand1DBinaryArray[randNumCount] == 1 && inhibitoryValid[(postNeuronAddr & 0x300) >> 8][preNeuronAddr & 0x3ff] == 0) //build a real synapse
					BuildSynapse(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr,
							exType, REAL_SYNAPSE, virtualNeuronAddrEnable);
				else if (inhibitoryValid[(postNeuronAddr & 0x300) >> 8][preNeuronAddr & 0x3ff] == 0)
					BuildSynapse(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr,
							exType, VIRTUAL_SYNAPSE, virtualNeuronAddrEnable);
				randNumCount += 1;
			}
		}
	}
	//feature1 to feature1
	virtualNeuronAddrEnable = 1;
	for (preNeuronId = 0; preNeuronId < FEATURE1_N*FEATURE1_LAYERS_N; preNeuronId++)
		for (postNeuronId = 0; postNeuronId < FEATURE1_N*FEATURE1_LAYERS_N; postNeuronId++) {
			if ((int)(preNeuronId/FEATURE1_N)!=(int)(postNeuronId/FEATURE1_N) && (preNeuronId%FEATURE1_N)==(postNeuronId%FEATURE1_N)) {
				preNeuronAddr = feature_layer1[preNeuronId];
				postNeuronAddr = feature_layer1[postNeuronId];
				coreId = inhibitoryVirtualNeuronCoreId[(postNeuronAddr & 0x300) >> 8][preNeuronAddr & 0xff];
				virtualNeuronAddr = ((preNeuronAddr & 0x3c00) >> 10) << 10 | coreId << 8 | (preNeuronAddr & 0xff);
				BuildSynapse(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr,
						(int16_t) (-1 * inType), REAL_SYNAPSE, virtualNeuronAddrEnable);
			}
		}
	virtualNeuronAddrEnable = 0;
	virtualNeuronAddr = 0;
	//feature1 to output2
	for (postNeuronId = 0; postNeuronId < OUTPUT2_N; postNeuronId++) {
		//generate random binary number 1D array
		int64_t rand1DBinaryArray[FEATURE1_N*FEATURE1_LAYERS_N];
		GetRand1DBinaryArray(rand1DBinaryArray, FEATURE1_N*FEATURE1_LAYERS_N, 60); //TOTAL_CAM_NUM_LEARNING
		randNumCount = 0;
		for (preNeuronId = 0; preNeuronId < FEATURE1_N*FEATURE1_LAYERS_N; preNeuronId++) {
			//randomly reset, depends on the ratio of total CAM number and OUTPUT1_N
			preNeuronAddr = feature_layer1[preNeuronId];
			postNeuronAddr = output_layer2[postNeuronId];
			if (rand1DBinaryArray[randNumCount] == 1 && (preNeuronAddr & 0x3ff) != 0) { //seems needed
				// && (preNeuronAddr & 0x3ff) != 1 && (preNeuronAddr & 0x3ff) != 2 && (preNeuronAddr & 0x3ff) != 3
				//all the teaching neuron addresses are selected from the first core. Need to be changed?
				BuildSynapse(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr,
						exType, REAL_SYNAPSE, virtualNeuronAddrEnable);
			} else
				BuildSynapse(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr,
						exType, VIRTUAL_SYNAPSE, virtualNeuronAddrEnable);
			randNumCount += 1;
		}
	}
	//output2 to output2 //CAM id 62 63 //!!!!!!!! can be reduced
//	virtualNeuronAddrEnable = 1;
	for (postNeuronId = 0; postNeuronId < OUTPUT2_N; postNeuronId++) {
		for (preNeuronId = 0; preNeuronId < OUTPUT2_N; preNeuronId++) {
			if (preNeuronId != postNeuronId) {
				preNeuronAddr = output_layer2[preNeuronId];
				postNeuronAddr = output_layer2[postNeuronId];
//				virtualNeuronAddr = ((preNeuronAddr & 0x300) >> 8) << 8 | (preNeuronAddr & 0xff);
				BuildSynapse(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr,
						(int16_t) (-1 * inType), REAL_SYNAPSE, virtualNeuronAddrEnable);
			}
		}
	}
	//feature1 to pooling1
/*	virtualNeuronAddr = 0;
	virtualNeuronAddrEnable = 0;
	for (preNeuronId = 0; preNeuronId < FEATURE1_N*FEATURE1_LAYERS_N; preNeuronId++)
		for (postNeuronId = 0; postNeuronId < POOLING1_N*POOLING1_LAYERS_N; postNeuronId++) {
			if ((int)((int)((preNeuronId%FEATURE1_N)/FEATURE1_L)/(int)(FEATURE1_L/POOLING1_L)) == (int)((postNeuronId%POOLING1_N)/POOLING1_L)
					&& (int)(((preNeuronId%FEATURE1_N)%FEATURE1_W)/(int)(FEATURE1_W/POOLING1_W)) == (postNeuronId%POOLING1_N)%POOLING1_W) {
				preNeuronAddr = feature_layer1[preNeuronId];
				postNeuronAddr = pooling_layer1[postNeuronId];
				BuildSynapse(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr,
						exType, REAL_SYNAPSE, virtualNeuronAddrEnable);
			}
		} */
	//pooling1 to pooling1
/*	for (preNeuronId = 0; preNeuronId < POOLING1_N*POOLING1_LAYERS_N; preNeuronId++)
		for (postNeuronId = 0; postNeuronId < POOLING1_N*POOLING1_LAYERS_N; postNeuronId++) {
			if ((int)(preNeuronId/POOLING1_N)!=(int)(postNeuronId/POOLING1_N) && (preNeuronId%POOLING1_N)==(postNeuronId%POOLING1_N)) {
				BuildSynapse(moduleData, eventSourceID, pooling_layer1[preNeuronId], pooling_layer1[postNeuronId], inType, REAL_SYNAPSE);
			}
		}
	//pooling1 to feature2
	for (postNeuronId = 0; postNeuronId < FEATURE2_N*FEATURE2_LAYERS_N; postNeuronId++) {
		//generate random binary number 1D array
		int64_t rand1DBinaryArray[FILTER2_N * POOLING1_LAYERS_N];
		GetRand1DBinaryArray(rand1DBinaryArray, FILTER2_N * POOLING1_LAYERS_N, TOTAL_CAM_NUM);
		randNumCount = 0;
		for (preNeuronId = 0; preNeuronId < POOLING1_N*POOLING1_LAYERS_N; preNeuronId++) {
			if ((int)((preNeuronId%POOLING1_N)/POOLING1_W) >= (int)((postNeuronId%FEATURE2_N)/FEATURE2_W) &&
					(int)((preNeuronId%POOLING1_N)/POOLING1_W) < (int)((postNeuronId%FEATURE2_N)/FEATURE2_W) + FILTER2_L &&
					(preNeuronId%POOLING1_N)%POOLING1_W >= (postNeuronId%FEATURE2_N)%FEATURE2_W &&
					(preNeuronId%POOLING1_N)%POOLING1_W < (postNeuronId%FEATURE2_N)%FEATURE2_W + FILTER2_W) {
				//randomly reset, depends on the ratio of total CAM number and FILTER2_N-FEATURE2_CAM_INHIBITORY_N
				if (rand1DBinaryArray[randNumCount] == 1)
					BuildSynapse(moduleData, eventSourceID, pooling_layer1[preNeuronId], feature_layer2[postNeuronId], exType, REAL_SYNAPSE);
				else
					BuildSynapse(moduleData, eventSourceID, pooling_layer1[preNeuronId], feature_layer2[postNeuronId], exType, VIRTUAL_SYNAPSE);
				randNumCount += 1;
			}
		}
	}
	//feature2 to feature2
	for (preNeuronId = 0; preNeuronId < FEATURE2_N*FEATURE2_LAYERS_N; preNeuronId++)
		for (postNeuronId = 0; postNeuronId < FEATURE2_N*FEATURE2_LAYERS_N; postNeuronId++) {
			if ((int)(preNeuronId/FEATURE2_N)!=(int)(postNeuronId/FEATURE2_N) && (preNeuronId%FEATURE2_N)==(postNeuronId%FEATURE2_N)) {
				BuildSynapse(moduleData, eventSourceID, feature_layer2[preNeuronId], feature_layer2[postNeuronId], inType, REAL_SYNAPSE);
			}
		}
	//feature2 to pooling2
	for (postNeuronId = 0; postNeuronId < POOLING2_N*POOLING2_LAYERS_N; postNeuronId++) {
		//generate random binary number 1D array
		int64_t rand1DBinaryArray[(FEATURE2_N/POOLING2_N)*FEATURE2_LAYERS_N];
		GetRand1DBinaryArray(rand1DBinaryArray, (FEATURE2_N/POOLING2_N)*FEATURE2_LAYERS_N, TOTAL_CAM_NUM);
		randNumCount = 0;
		for (preNeuronId = 0; preNeuronId < FEATURE2_N*FEATURE2_LAYERS_N; preNeuronId++) {
			if ((int)((int)((preNeuronId%FEATURE2_N)/FEATURE2_L)/(int)(FEATURE2_L/POOLING2_L)) == (int)((postNeuronId%POOLING2_N)/POOLING2_L)
					&& (int)(((preNeuronId%FEATURE2_N)%FEATURE2_W)/(int)(FEATURE2_W/POOLING2_W)) == (postNeuronId%POOLING2_N)%POOLING2_W) {
				//randomly reset, depends on the ratio of total CAM number and (FEATURE2_N/POOLING2_N)*FEATURE2_LAYERS_N-POOLING2_CAM_INHIBITORY_N
				if (rand1DBinaryArray[randNumCount] == 1)
					BuildSynapse(moduleData, eventSourceID, feature_layer2[preNeuronId], pooling_layer2[postNeuronId], exType, REAL_SYNAPSE);
				else
					BuildSynapse(moduleData, eventSourceID, feature_layer2[preNeuronId], pooling_layer2[postNeuronId], exType, VIRTUAL_SYNAPSE);
				randNumCount += 1;
			}
		}
	}
	//pooling2 to pooling2
	for (preNeuronId = 0; preNeuronId < POOLING2_N*POOLING2_LAYERS_N; preNeuronId++)
		for (postNeuronId = 0; postNeuronId < POOLING2_N*POOLING2_LAYERS_N; postNeuronId++) {
			if ((int)(preNeuronId/POOLING2_N)!=(int)(postNeuronId/POOLING2_N) && (preNeuronId%POOLING2_N)==(postNeuronId%POOLING2_N)) {
				BuildSynapse(moduleData, eventSourceID, pooling_layer2[preNeuronId], pooling_layer2[postNeuronId], inType, REAL_SYNAPSE);
			}
		}
	//pooling2 to output1
	for (postNeuronId = 0; postNeuronId < OUTPUT1_N; postNeuronId++) {
		//generate random binary number 1D array
		int64_t rand1DBinaryArray[POOLING2_N*POOLING2_LAYERS_N];
		GetRand1DBinaryArray(rand1DBinaryArray, POOLING2_N*POOLING2_LAYERS_N, TOTAL_CAM_NUM);
		randNumCount = 0;
		for (preNeuronId = 0; preNeuronId < POOLING2_N*POOLING2_LAYERS_N; preNeuronId++) {
			//randomly reset, depends on the ratio of total CAM number and POOLING2_N*POOLING2_LAYERS_N
			if (rand1DBinaryArray[randNumCount] == 1)
				BuildSynapse(moduleData, eventSourceID, pooling_layer2[preNeuronId], output_layer1[postNeuronId], exType, REAL_SYNAPSE);
			else
				BuildSynapse(moduleData, eventSourceID, pooling_layer2[preNeuronId], output_layer1[postNeuronId], exType, VIRTUAL_SYNAPSE);
			randNumCount += 1;
		}
	}
	//output1 to output2
	for (postNeuronId = 0; postNeuronId < OUTPUT2_N; postNeuronId++) {
		//generate random binary number 1D array
		int64_t rand1DBinaryArray[OUTPUT1_N];
		GetRand1DBinaryArray(rand1DBinaryArray, OUTPUT1_N, TOTAL_CAM_NUM);
		randNumCount = 0;
		for (preNeuronId = 0; preNeuronId < OUTPUT1_N; preNeuronId++) {
			//randomly reset, depends on the ratio of total CAM number and OUTPUT1_N
			if (rand1DBinaryArray[randNumCount] == 1)
				BuildSynapse(moduleData, eventSourceID, output_layer1[preNeuronId], output_layer2[postNeuronId], exType, REAL_SYNAPSE);
			else
				BuildSynapse(moduleData, eventSourceID, output_layer1[preNeuronId], output_layer2[postNeuronId], exType, VIRTUAL_SYNAPSE);
			randNumCount += 1;
		}
	} */

//	ResetBiases(moduleData, eventSourceID);
//	EnableStimuliGen(moduleData, eventSourceID, 4);
//	stimulating = 1;

	SetInputLayerCam(moduleData, eventSourceID); //It's different thread, should be put in the end.

	return (true);
}

//build synapses when reseting
bool BuildSynapse(caerModuleData moduleData, int16_t eventSourceID, uint32_t preNeuronAddr, uint32_t postNeuronAddr, uint32_t virtualNeuronAddr,
		int16_t synapseType, int8_t realOrVirtualSynapse, int8_t virtualNeuronAddrEnable)
{
	uint32_t sramId, camId, sram_id, cam_id;
	int chipCoreId;
	int sramFound, camFound;
	int sramAvailable;
//	int output_disabled;
//	uint32_t i;
	//for SRAM
	if (realOrVirtualSynapse != EXTERNAL_REAL_SYNAPSE) {
		sramFound = 0;
		for(sram_id = 0; sram_id < TOTAL_SRAM_NUM; sram_id++) { //search for available SRAM
			chipCoreId = (int) (postNeuronAddr >> 8);
			if(memory.sramMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][sram_id] == 1 &&
							memory.sramMapContent->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][sram_id] == chipCoreId) { //start the searching from second SRAM, for visualization
				sramFound = 1;
			}
		}
		if (sramFound == 0) {
			sramAvailable = 0;
			for(sram_id = 0; sram_id < TOTAL_SRAM_NUM; sram_id++) { //search for available SRAM
				if (sramAvailable == 0 && memory.sramMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(sram_id + 1) % TOTAL_SRAM_NUM] == 0) {
					sramAvailable = 1;
					sramId = (sram_id + 1) % TOTAL_SRAM_NUM; //(sram_id + 1) % TOTAL_SRAM_NUM; keep the SRAM for viewer
				}
			}
			if (sramAvailable == 1 && sramId != 0) { //sramId != 0 && sramId != 1 && sramId != 2 && sramId != 3
				WriteSram(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr, sramId, virtualNeuronAddrEnable);
				memory.sramMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][sramId] = 1; //taken
				chipCoreId = (int) (postNeuronAddr >> 8);
				memory.sramMapContent->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][sramId] = chipCoreId; //taken
			}
		}
	}
	//for CAM
	camFound = 0;
	for(cam_id = 0; cam_id < TOTAL_CAM_NUM; cam_id++) { //search for available CAM
		if (synapseType > 0) {
			if (memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][cam_id] == (int32_t) preNeuronAddr) {
				camFound = 1;
				break;
			}
		} else {
			if (memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][cam_id] == (int32_t) preNeuronAddr ||
					(memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][cam_id] == (int32_t) virtualNeuronAddr &&
							virtualNeuronAddrEnable == 1)) { //to change
				camFound = 1;
				break;
			}
		}
		if (memory.camMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][cam_id] == 0){
			camId = cam_id;
			break;
		}
	}

	if (realOrVirtualSynapse == REAL_SYNAPSE || realOrVirtualSynapse == EXTERNAL_REAL_SYNAPSE) {
		if (camFound == 0) {
/*			output_disabled = 0;
			if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
				for (i = 0; i < 3; i++) {
					if (memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][i] == 1 && (postNeuronAddr & 0xff) != i) {
						output_disabled = 1;
						break;
					}
				}
				if (output_disabled == 0) {
					WriteCam(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr, camId, synapseType, virtualNeuronAddrEnable);
					memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr & 0xff] = 1;
				}
			} else {
				WriteCam(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr, camId, synapseType, virtualNeuronAddrEnable);
			} */
			WriteCam(moduleData, eventSourceID, preNeuronAddr, postNeuronAddr, virtualNeuronAddr, camId, synapseType, virtualNeuronAddrEnable);
			memory.camMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = (int32_t) preNeuronAddr;
			if (synapseType > 0) {
				memory.camSize->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][0] += 1;
			}
		}
	}
	//memories for the chip
	if (synapseType > 0) { //if it is EX synapse
		int32_t memoryId = memory.filterMapSize->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][0];
		memory.filterMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][memoryId] = (int32_t) preNeuronAddr;
		memory.connectionCamMap->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][memoryId] = (int32_t) camId;
		memory.filterMapSize->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][0] += 1;
		if (realOrVirtualSynapse != EXTERNAL_REAL_SYNAPSE) {
			memory.connectionMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = 1; //there is an EX connection
			memory.weightMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = 1; //8 initial weight
			if (realOrVirtualSynapse == REAL_SYNAPSE) {
				memory.synapseMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET] = synapseType; //1 should be synapseType;
				if (camFound == 0) {
					memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = synapseType;
					memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = (int32_t) preNeuronAddr;
					if ((postNeuronAddr & 0x3c00) >> 10 == 3 && (postNeuronAddr & 0x300) >> 8 == 3) {
						memory.outputMap->buffer2d[preNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][(postNeuronAddr & 0xff)/6] = 1;
					}
				}
			}
		}
	} else {
		memory.camMapContentType->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = synapseType;
		memory.camMapContentSource->buffer2d[postNeuronAddr-MEMORY_NEURON_ADDR_OFFSET][camId] = (int32_t) preNeuronAddr; //(3 << 8 | (preNeuronAddr & 0xff)); //to change
	}
	return (true);
}

//write neuron CAM when a synapse is built or modified
bool WriteCam(caerModuleData moduleData, int16_t eventSourceID, uint32_t preNeuronAddr, uint32_t postNeuronAddr, uint32_t virtualNeuronAddr,
		uint32_t camId, int16_t synapseType, int8_t virtualNeuronAddrEnable) {

	LFilterState state = moduleData->moduleState;

	// --- start  usb handle / from spike event source id
	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(eventSourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));
	caerInputDynapseState stateSource = state->eventSourceModuleState;
	// --- end usb handle

	uint32_t chipId_t, chipId, bits;
	chipId_t = postNeuronAddr >> NEURON_CHIPID_SHIFT;
	if (chipId_t == 1)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U0;
	else if (chipId_t == 2)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U1;
	else if (chipId_t == 3)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U2;
	else if (chipId_t == 4)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U3;
	uint32_t ei = 0;
	uint32_t fs = 0;
    uint32_t address = preNeuronAddr & NEURON_ADDRESS_BITS;
    uint32_t source_core = 0;
    if (virtualNeuronAddrEnable == 0)
    	source_core = (preNeuronAddr & NEURON_COREID_BITS) >> NEURON_COREID_SHIFT;
    else
    	source_core = (virtualNeuronAddr & NEURON_COREID_BITS) >> NEURON_COREID_SHIFT; //to change
    if (synapseType > 0) //if it is EX synapse
    	ei = EXCITATORY_SYNAPSE;
    else
    	ei = INHIBITORY_SYNAPSE;
    if (abs(synapseType) == FAST_SYNAPSE_ID)
    	fs = FAST_SYNAPSE;
    else if (abs(synapseType) == SLOW_SYNAPSE_ID)
    	fs = SLOW_SYNAPSE;
    else if (abs(synapseType) == NO_SYNAPSE_ID) {
        address = NO_SYNAPSE_ADDRESS;
        source_core = NO_SYNAPSE_CORE;
    }
    uint32_t coreId = (postNeuronAddr & NEURON_COREID_BITS) >> NEURON_COREID_SHIFT;
    uint32_t neuron_row = (postNeuronAddr & NEURON_ROW_BITS) >> NEURON_ROW_SHIFT;
    uint32_t synapse_row = camId;
    uint32_t row = neuron_row << CAM_NEURON_ROW_SHIFT | synapse_row;
	uint32_t column = postNeuronAddr & NEURON_COL_BITS;
    bits = ei << CXQ_CAM_EI_SHIFT |
    		fs << CXQ_CAM_FS_SHIFT |
    		address << CXQ_ADDR_SHIFT |
    		source_core << CXQ_SOURCE_CORE_SHIFT |
    		CXQ_PROGRAM |
    		coreId << CXQ_PROGRAM_COREID_SHIFT |
    		row << CXQ_PROGRAM_ROW_SHIFT |
    		column << CXQ_PROGRAM_COLUMN_SHIFT;
	caerDeviceConfigSet(stateSource->deviceState, DYNAPSE_CONFIG_CHIP, DYNAPSE_CONFIG_CHIP_ID, chipId);
	caerDeviceConfigSet(stateSource->deviceState, DYNAPSE_CONFIG_CHIP, DYNAPSE_CONFIG_CHIP_CONTENT, bits); //this is the 30 bits

	return (true);
}

//write neuron SRAM when a synapse is built or modified
bool WriteSram(caerModuleData moduleData, int16_t eventSourceID, uint32_t preNeuronAddr, uint32_t postNeuronAddr, uint32_t virtualNeuronAddr,
		uint32_t sramId, int8_t virtualNeuronAddrEnable) {
//	caerDeviceHandle usb_handle = ((caerInputDynapseState) moduleData->moduleState)->deviceState;
	LFilterState state = moduleData->moduleState;

	// --- start  usb handle / from spike event source id
	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(eventSourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));
	caerInputDynapseState stateSource = state->eventSourceModuleState;
	// --- end usb handle

	uint32_t chipId, bits;
	chipId = preNeuronAddr >> NEURON_CHIPID_SHIFT;
	if (chipId == 1)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U0;
	else if (chipId == 2)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U1;
	else if (chipId == 3)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U2;
	else if (chipId == 4)
		chipId = DYNAPSE_CONFIG_DYNAPSE_U3;
	uint32_t virtual_coreId = 0;
	if (virtualNeuronAddrEnable == 0)
		virtual_coreId = (preNeuronAddr & NEURON_COREID_BITS) >> NEURON_COREID_SHIFT;
	else
		virtual_coreId = (virtualNeuronAddr & NEURON_COREID_BITS) >> NEURON_COREID_SHIFT;; //to change
	uint32_t source_chipId = (preNeuronAddr >> NEURON_CHIPID_SHIFT) - 1; //for calculation
	uint32_t destination_chipId = (postNeuronAddr >> NEURON_CHIPID_SHIFT) - 1; //for calculation
	uint32_t sy, dy, sx, dx;
    if ((source_chipId / BOARD_HEIGHT) >= (destination_chipId / BOARD_HEIGHT))
        sy = EVENT_DIRECTION_Y_DOWN; //EVENT_DIRECTION_Y_UP;
    else
        sy = EVENT_DIRECTION_Y_DOWN;
    if ((source_chipId % BOARD_WIDTH) <= (destination_chipId % BOARD_WIDTH))
        sx = EVENT_DIRECTION_X_RIGHT; //EVENT_DIRECTION_X_RIGHT;
    else
        sx = EVENT_DIRECTION_X_LEFT; //EVENT_DIRECTION_X_LEFT;
    if (source_chipId == destination_chipId)
    	dy = 0;
    else
    	dy = 1; //(uint32_t) abs((int32_t)(source_chipId / BOARD_HEIGHT) - (int32_t)(destination_chipId / BOARD_HEIGHT));
    dx = 0; //(uint32_t) abs((int32_t)(source_chipId % BOARD_WIDTH) - (int32_t)(destination_chipId % BOARD_WIDTH));
    uint32_t dest_coreId = (uint32_t) (1 << ((postNeuronAddr & NEURON_COREID_BITS) >> NEURON_COREID_SHIFT));
    uint32_t coreId = (preNeuronAddr & NEURON_COREID_BITS) >> NEURON_COREID_SHIFT;
    uint32_t neuron_row = (preNeuronAddr & NEURON_ROW_BITS) >> NEURON_ROW_SHIFT;
    uint32_t neuron_column = preNeuronAddr & NEURON_COL_BITS;
    uint32_t synapse_row = sramId;
    uint32_t row = neuron_row << SRAM_NEURON_ROW_SHIFT | neuron_column << SRAM_NEURON_COL_SHIFT | synapse_row; //synapse_row 0 1 cleared why?
    uint32_t column = SRAM_COL_VALUE;
    bits = virtual_coreId << CXQ_SRAM_VIRTUAL_SOURCE_CORE_SHIFT |
    		sy << CXQ_SRAM_SY_SHIFT |
    		dy << CXQ_SRAM_DY_SHIFT |
    		sx << CXQ_SRAM_SX_SHIFT |
    		dx << CXQ_SRAM_DX_SHIFT |
    		dest_coreId << CXQ_SRAM_DEST_CORE_SHIFT |
    		CXQ_PROGRAM |
    		coreId << CXQ_PROGRAM_COREID_SHIFT |
    		row << CXQ_PROGRAM_ROW_SHIFT |
    		column << CXQ_PROGRAM_COLUMN_SHIFT;
	caerDeviceConfigSet(stateSource->deviceState, DYNAPSE_CONFIG_CHIP, DYNAPSE_CONFIG_CHIP_ID, chipId);
	caerDeviceConfigSet(stateSource->deviceState, DYNAPSE_CONFIG_CHIP, DYNAPSE_CONFIG_CHIP_CONTENT, bits); //this is the 30 bits
	return(true);
}

bool ClearAllCam(caerModuleData moduleData, int16_t eventSourceID) {
	uint32_t neuronId, camId;
	for (neuronId = 0; neuronId < 32 * 32; neuronId++) {
		for (camId = 0; camId < 64; camId++) {
			WriteCam(moduleData, eventSourceID, 0, 1 << 10 | neuronId, 0, camId, 0, 0); //1 2 3 4
			WriteCam(moduleData, eventSourceID, 0, 2 << 10 | neuronId, 0, camId, 0, 0);
			WriteCam(moduleData, eventSourceID, 0, 3 << 10 | neuronId, 0, camId, 0, 0);
			WriteCam(moduleData, eventSourceID, 0, 4 << 10 | neuronId, 0, camId, 0, 0);
		}
	}
	return (true);
}

void Shuffle1DArray(int64_t *array, int64_t Range) {
	if (Range > 1) {
		int64_t i;
		for (i = 0; i < Range; i++) {
			int64_t j = i + rand() / (RAND_MAX / (Range - i) + 1);
			int64_t t = array[j];
			array[j] = array[i];
			array[i] = t;
		}
	}
}

bool EnableStimuliGen(caerModuleData moduleData, int16_t eventSourceID, int32_t pattern) {
	LFilterState state = moduleData->moduleState;

	// --- start  usb handle / from spike event source id
	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(eventSourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));
//	caerInputDynapseState stateSource = state->eventSourceModuleState;
	// --- end usb handle

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");
//	sshsNodePutShort(spikeNode, "dataSizeX", DYNAPSE_X4BOARD_NEUX);
	sshsNodePutBool(spikeNode, "running", true);
	sshsNodePutInt(spikeNode, "stim_type", pattern);
	sshsNodePutInt(spikeNode, "stim_duration", 100);
	sshsNodePutInt(spikeNode, "stim_avr", 1); //2000
	sshsNodePutBool(spikeNode, "repeat", true);
	sshsNodePutBool(spikeNode, "doStim", true);
	return (true);
}

bool DisableStimuliGen(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	// --- start  usb handle / from spike event source id
	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(eventSourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));
//	caerInputDynapseState stateSource = state->eventSourceModuleState;
	// --- end usb handle

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");
//	sshsNodePutShort(spikeNode, "dataSizeX", DYNAPSE_X4BOARD_NEUX);
	sshsNodePutBool(spikeNode, "running", true);
	sshsNodePutBool(spikeNode, "doStim", false);
	return (true);
}

bool EnableStimuliGenPrimitiveCam(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");

	sshsNodePutBool(spikeNode, "doStimPrimitiveCam", true);
	return (true);
}

bool DisableStimuliGenPrimitiveCam(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");

	sshsNodePutBool(spikeNode, "doStimPrimitiveCam", false);
	return (true);
}

bool EnableTeachingSignal(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");

	sshsNodePutBool(spikeNode, "sendInhibitoryStimuli", false);
	sshsNodePutBool(spikeNode, "sendTeachingStimuli", true);
	return (true);
}

bool DisableTeachingSignal(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");

	sshsNodePutBool(spikeNode, "sendInhibitoryStimuli", true);
	sshsNodePutBool(spikeNode, "sendTeachingStimuli", false);
	return (true);
}

bool EnableTeaching(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");

	sshsNodePutBool(spikeNode, "teaching", true);
	return (true);
}

bool DisableTeaching(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");

	sshsNodePutBool(spikeNode, "teaching", false);
	return (true);
}

bool SetInputLayerCam(caerModuleData moduleData, int16_t eventSourceID) {
/*	LFilterState state = moduleData->moduleState;

	// --- start  usb handle / from spike event source id
	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(eventSourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));
	// --- end usb handle

	sshsNode deviceConfigNodeMain = sshsGetRelativeNode(state->eventSourceConfigNode, chipIDToName(DYNAPSE_CHIP_DYNAPSE, true));
	sshsNode spikeNode = sshsGetRelativeNode(deviceConfigNodeMain, "spikeGen/");
	sshsNodePutBool(spikeNode, "running", true);
	sshsNodePutBool(spikeNode, "setCamSingle", true);
	return (true); */

	int64_t rowId, colId;
	int64_t num = DYNAPSE_CONFIG_CAMNUM;
	// generate pattern A
	uint32_t spikePatternA[DYNAPSE_CONFIG_XCHIPSIZE][DYNAPSE_CONFIG_YCHIPSIZE];
	int cx, cy, r;
	cx = 16;
	cy = 16;
	r = 14;
	for (rowId = 0; rowId < DYNAPSE_CONFIG_XCHIPSIZE; rowId++)
		for (colId = 0; colId < DYNAPSE_CONFIG_YCHIPSIZE; colId++)
			spikePatternA[rowId][colId] = 0;
	for (rowId = cx - r; rowId <= cx + r; rowId++)
		for (colId = cy - r; colId <= cy + r; colId++)
			if (((cx - rowId) * (cx - rowId)
					+ (cy - colId) * (cy - colId) <= r * r + sqrt(r))
					&& ((cx - rowId) * (cx - rowId)
							+ (cy - colId) * (cy - colId) >= r * r - r))
				spikePatternA[rowId][colId] = 1;

	uint32_t spikePatternB[DYNAPSE_CONFIG_XCHIPSIZE][DYNAPSE_CONFIG_YCHIPSIZE];
	for (rowId = -num; rowId < num; rowId++) {
		for (colId = -num; colId < num; colId++) {
			if (abs((int) rowId) + abs((int) colId) == num) // Change this condition >= <=
				spikePatternB[rowId + DYNAPSE_CONFIG_CAMNUM][colId + DYNAPSE_CONFIG_CAMNUM] = 1;
			else
				spikePatternB[rowId + DYNAPSE_CONFIG_CAMNUM][colId + DYNAPSE_CONFIG_CAMNUM] = 0;
		}
	}

	uint32_t spikePatternC[DYNAPSE_CONFIG_XCHIPSIZE][DYNAPSE_CONFIG_YCHIPSIZE];
	for (rowId = -num; rowId < num; rowId++) {
		for (colId = -num; colId < num; colId++) {
			if (abs((int) rowId) == abs((int) colId)) // Change this condition
				spikePatternC[rowId + DYNAPSE_CONFIG_CAMNUM][colId + DYNAPSE_CONFIG_CAMNUM] = 1;
			else
				spikePatternC[rowId + DYNAPSE_CONFIG_CAMNUM][colId + DYNAPSE_CONFIG_CAMNUM] = 0;
		}
	}

	uint32_t neuronId;
//	uint32_t sourceNeuronId;
//	caerLog(CAER_LOG_NOTICE, "\nSpikeGen", "Started programming cam..");
	for (rowId = 0; rowId < DYNAPSE_CONFIG_XCHIPSIZE; rowId++) {
		for (colId = 0; colId < DYNAPSE_CONFIG_YCHIPSIZE; colId++) {
			neuronId = (uint32_t) (1 << 10 | ((rowId & 0X10) >> 4) << 9 | ((colId & 0X10) >> 4) << 8 |(rowId & 0xf) << 4 | (colId & 0xf));
			if (spikePatternA[rowId][colId] == 1)
				WriteCam(moduleData, eventSourceID, 1, neuronId, 0, 0, 2, 0);
			if (spikePatternB[rowId][colId] == 1)
				WriteCam(moduleData, eventSourceID, 2, neuronId, 0, 1, 2, 0);
			if (spikePatternC[rowId][colId] == 1)
				WriteCam(moduleData, eventSourceID, 3, neuronId, 0, 2, 2, 0);
		}
	}

//	sourceNeuronId = 3 << 8 | 0; //1
//	sourceNeuronId = 3 << 10 | 3 << 8 | 0;
	neuronId = 3 << 10 | 3 << 8 | 0;
//	WriteCam(moduleData, eventSourceID, 1, neuronId, 0, 61, 2, 0);
//	WriteCam(moduleData, eventSourceID, 2, neuronId, 0, 62, -2, 0);
//	WriteCam(moduleData, eventSourceID, 3, neuronId, 0, 63, -2, 0);
	WriteCam(moduleData, eventSourceID, neuronId, neuronId, 0, 62, 2, 0); //!!!!!!!! can be changed back
	WriteCam(moduleData, eventSourceID, 3 << 8 | 3, neuronId, 0, 63, -2, 0);
//	sourceNeuronId = 3 << 8 | 1; //2
//	sourceNeuronId = 3 << 10 | 3 << 8 | 7;
	neuronId = 3 << 10 | 3 << 8 | 6; //neuronId = 3 << 10 | 3 << 8 | 1;
//	WriteCam(moduleData, eventSourceID, 1, neuronId, 0, 61, -2, 0);
//	WriteCam(moduleData, eventSourceID, 2, neuronId, 0, 62, 2, 0);
//	WriteCam(moduleData, eventSourceID, 3, neuronId, 0, 63, -2, 0);
	WriteCam(moduleData, eventSourceID, neuronId, neuronId, 0, 62, 2, 0);
	WriteCam(moduleData, eventSourceID, 3 << 8 | 3, neuronId, 0, 63, -2, 0);
//	sourceNeuronId = 3 << 8 | 2; //3
//	sourceNeuronId = 3 << 10 | 3 << 8 | 14;
	neuronId = 3 << 10 | 3 << 8 | 12; //neuronId = 3 << 10 | 3 << 8 | 2;
//	WriteCam(moduleData, eventSourceID, 1, neuronId, 0, 61, -2, 0);
//	WriteCam(moduleData, eventSourceID, 2, neuronId, 0, 62, -2, 0);
//	WriteCam(moduleData, eventSourceID, 3, neuronId, 0, 63, 2, 0);
	WriteCam(moduleData, eventSourceID, neuronId, neuronId, 0, 62, 2, 0);
	WriteCam(moduleData, eventSourceID, 3 << 8 | 3, neuronId, 0, 63, -2, 0);

//	caerLog(CAER_LOG_NOTICE, "\nSpikeGen", "CAM programmed successfully.");

	return (true);
}

COLOUR GetColourW(double v, double vmin, double vmax)
{
	COLOUR c = {0,0,0}; //{65535, 65535, 65535}; // white
	double dv;
	double value;

	if (v < vmin)
		v = vmin;
	if (v > vmax)
		v = vmax;
	dv = vmax - vmin;

	if (v < (vmin + dv / 4)) {
		c.r = 0;
		value = ( 4 * (v - vmin) / dv ) * 65535;
		if (value > 30000)
			c.g = 30000;
		else if (value < 0)
			c.g = 0;
		else
			c.g = (uint16_t) value;
	} else if (v < (vmin + dv / 2)) {
		c.r = 0;
		value = (1 + 4 * (vmin + dv / 4 - v) / dv) * 65535;
		if (value > 30000)
			c.b = 30000;
		else if (value < 0)
			c.b = 0;
		else
			c.b = (uint16_t) value;
	} else if (v < (vmin + dv * 3 / 4)) {
		c.b = 0;
		value = (4 * (v - vmin - dv / 2) / dv) * 65535;
		if (value > 30000)
			c.r = 30000;
		else if (value < 0)
			c.r = 0;
		else
			c.r = (uint16_t) value;
	} else {
		c.b = 0;
		value = (4 * (v - vmin - dv / 2) / dv) * 65535;
		if (value > 30000)
			c.r = 30000;
		else if (value < 0)
			c.r = 0;
		else
			c.r = (uint16_t) value;
	}
	return(c);
}

COLOUR GetColourS(int v) //, double vmin, double vmax
{
	COLOUR c = {0,0,0};
	if (v == 0) { //black
		c.r = 0;
		c.g = 0;
		c.b = 0;
	} else if (0 < v && v <= 128) { //v <= 128
		c.r = (uint16_t) ((v & 0x7) * 30);
		c.g = (uint16_t) (((v & 0x38) >> 3) * 30);
		c.b = (uint16_t) (((v & 0x1c0) >> 6) * 30);
	} else {
		c.r = 255;
		c.g = 255;
		c.b = 255;
	}
	c.r = (uint16_t) (c.r * 257);
	c.g = (uint16_t) (c.g * 257);
	c.b = (uint16_t) (c.b * 257);
	return(c);
}
/*
COLOUR GetColourS(int v) //, double vmin, double vmax
{
	COLOUR c = {0,0,0};
	if (v == 0) { //black
		c.r = 255;
		c.g = 255;
		c.b = 255;
	} else if (v <= 128) {
		c.r = (uint16_t) ((v & 0x7) * 30);
		c.g = (uint16_t) (((v & 0x38) >> 3) * 30);
		c.b = (uint16_t) (((v & 0x1c0) >> 6) * 30);
	} else {
		c.r = 0;
		c.g = 0;
		c.b = 0;
	}
	c.r = (uint16_t) (c.r * 257);
	c.g = (uint16_t) (c.g * 257);
	c.b = (uint16_t) (c.b * 257);
	return(c);
}
*/
bool ResetBiases(caerModuleData moduleData, int16_t eventSourceID) {
	LFilterState state = moduleData->moduleState;

	// --- start  usb handle / from spike event source id
	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(eventSourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));
	caerInputDynapseState stateSource = state->eventSourceModuleState;
	// --- end usb handle

	uint32_t chipId_t, chipId, coreId;

	for (chipId_t = 0; chipId_t < 4; chipId_t++) { //1 4

		if (chipId_t == 0)
			chipId = DYNAPSE_CONFIG_DYNAPSE_U0;
		else if (chipId_t == 1)
			chipId = DYNAPSE_CONFIG_DYNAPSE_U1;
		else if (chipId_t == 2)
			chipId = DYNAPSE_CONFIG_DYNAPSE_U2;
		else if (chipId_t == 3)
			chipId = DYNAPSE_CONFIG_DYNAPSE_U3;

		caerDeviceConfigSet(stateSource->deviceState, DYNAPSE_CONFIG_CHIP, DYNAPSE_CONFIG_CHIP_ID, chipId);

		for (coreId = 0; coreId < 4; coreId++) {
			//sweep all the biases
		    // select right bias name
			//caer-ctrl
			//put /1/1-DYNAPSEFX2/DYNAPSE_CONFIG_DYNAPSE_U1/bias/C0_IF_DC_P/ coarseValue byte 6
			if (chipId == DYNAPSE_CONFIG_DYNAPSE_U0) {
				if (coreId == 0) {
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 200, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 2, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 200, "HighBias", "PBias"); //105
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 0, 76, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
				} else if (coreId == 3) {
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 200, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 3, 40, "HighBias", "NBias"); //2, 40
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 105, "HighBias", "PBias"); //105
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 0, 76, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
				} else {
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 200, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 2, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 105, "HighBias", "PBias"); //105
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 0, 76, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
				}
			} else if (chipId == DYNAPSE_CONFIG_DYNAPSE_U2) { // DYNAPSE_CONFIG_DYNAPSE_U2 = 4
				if (coreId == 0) {
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 200, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 2, 35, "HighBias", "NBias"); //3, 150 //4, 40
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 70, "HighBias", "PBias"); //6, 200 //105
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 6, 200, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 6, 105, "HighBias", "PBias"); //7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 0, 150, "HighBias", "PBias"); //7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 1, 250, "HighBias", "NBias"); //29!!!
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 1, 100, "HighBias", "NBias"); //19!!! //0, 38
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 0, 200, "HighBias", "NBias"); //7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
				} else if (coreId == 1) {
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 200, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 3, 21, "HighBias", "NBias"); //17!!! //3, 150 //4, 40
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 40, "HighBias", "PBias"); //6, 200 //105
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 6, 200, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 250, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 6, 105, "HighBias", "PBias"); //7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 0, 150, "HighBias", "PBias"); //7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 0, 250, "HighBias", "NBias"); //28!!!
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 0, 100, "HighBias", "NBias"); //19!!! //0, 38
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 0, 200, "HighBias", "NBias"); //7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
				} else if (coreId == 2) { //slow ex ok
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 245, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 3, 60, "HighBias", "NBias"); //10!!! //3, 150 //4, 40
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 60, "HighBias", "PBias"); //6, 200 //105
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 6, 200, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 250, "HighBias", "PBias"); //220!!!
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 6, 105, "HighBias", "PBias"); //7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 0, 150, "HighBias", "PBias"); //7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 0, 250, "HighBias", "NBias"); //237!!!
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 0, 150, "HighBias", "NBias"); //0, 38
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 0, 200, "HighBias", "NBias"); //7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
				} else if (coreId == 3) {
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 200, "LowBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 1, 150, "HighBias", "NBias"); //3, 150 //4, 40
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 100, "HighBias", "PBias"); //6, 200 //105
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 6, 200, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 0, 220, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 6, 105, "HighBias", "PBias"); //7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 0, 250, "HighBias", "PBias"); //7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 0, 76, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 0, 19, "HighBias", "NBias"); //0, 38
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 0, 250, "HighBias", "NBias"); //7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
					setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
				}
			} else {
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTAU_N", 7, 35, "LowBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHTHR_N", 7, 0, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_AHW_P", 7, 0, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_BUF_P", 3, 80, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_CASC_N", 7, 0, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_DC_P", 7, 0, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_NMDA_N", 7, 0, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_RFR_N", 5, 255, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU1_N", 4, 200, "LowBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_TAU2_N", 6, 15, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "IF_THR_N", 4, 40, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_F_P", 6, 105, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_TAU_S_P", 7, 40, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_F_P", 0, 220, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPIE_THR_S_P", 7, 0, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_F_P", 6, 105, "HighBias", "PBias"); //7, 40, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_TAU_S_P", 7, 40, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_F_P", 0, 220, "HighBias", "PBias"); //7, 40, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "NPDPII_THR_S_P", 7, 40, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_F_N", 0, 76, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_EXC_S_N", 7, 0, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_F_N", 0, 76, "HighBias", "NBias"); //7, 0, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "PS_WEIGHT_INH_S_N", 7, 0, "HighBias", "NBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "PULSE_PWLK_P", 3, 50, "HighBias", "PBias");
				setBiasBits(moduleData, eventSourceID, chipId, coreId, "R2R_P", 4, 85, "HighBias", "PBias");
			}

		}
	}
	return (true);
}

void setBiasBits(caerModuleData moduleData, int16_t eventSourceID, uint32_t chipId, uint32_t coreId, const char *biasName_t,
		uint8_t coarseValue, uint16_t fineValue, const char *lowHigh, const char *npBias) {
	LFilterState state = moduleData->moduleState;

	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(eventSourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(eventSourceID));
	caerInputDynapseState stateSource = state->eventSourceModuleState;

	struct caer_dynapse_info dynapse_info = caerDynapseInfoGet(stateSource->deviceState);

    size_t biasNameLength = strlen(biasName_t);
    char biasName[biasNameLength+3];

	biasName[0] = 'C';
	if (coreId == 0)
		biasName[1] = '0';
	else if (coreId == 1)
		biasName[1] = '1';
	else if (coreId == 2)
		biasName[1] = '2';
	else if (coreId == 3)
		biasName[1] = '3';
	biasName[2] = '_';

	uint32_t i;
	for(i = 0; i < biasNameLength + 3; i++) {
		biasName[3+i] = biasName_t[i];
	}
	uint32_t bits = generatesBitsCoarseFineBiasSetting(state->eventSourceConfigNode, &dynapse_info,
			biasName, coarseValue, fineValue, lowHigh, "Normal", npBias, true, chipId);

	caerDeviceConfigSet(stateSource->deviceState, DYNAPSE_CONFIG_CHIP, DYNAPSE_CONFIG_CHIP_CONTENT, bits);
}

void SetTimer() {
  struct itimerval itv;
  itv.it_interval.tv_sec = 1;
  itv.it_interval.tv_usec = 0;
  itv.it_value.tv_sec = 1;
  itv.it_value.tv_usec = 0;
  setitimer(ITIMER_REAL, &itv, &oldtv);
}

void SignalHandler(int m) {
	time_count = (time_count + 1) % 4294967295;
//	printf("%d\n", time_count);
//	printf("%d\n", m);
}

void GetRand1DArray(int64_t *array, int64_t Range, int64_t CamNumAvailable) {
	int64_t temp[Range]; //sizeof(array) doesn't work
	int64_t i;
	for (i = 0; i < Range; i++) {
		temp[i] = i;
	}
	Shuffle1DArray(temp, Range);
	for (i = 0; i < CamNumAvailable; i++) {
		array[i] = temp[i];
	}
}
void GetRand1DBinaryArray(int64_t *binaryArray, int64_t Range, int64_t CamNumAvailable) {
	int64_t array[CamNumAvailable];
	GetRand1DArray(array, Range, CamNumAvailable);
	int64_t i;
	int64_t num;
	for (i = 0; i < Range; i++) {
		binaryArray[i] = 0;
	}
	for (i = 0; i < CamNumAvailable; i++) {
		num = array[i];
		binaryArray[num] = 1;
	}
}
