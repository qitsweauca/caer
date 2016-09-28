/* NullHop Interface for CNN acceleration
 *  Author: federico.corradi@inilabs.com
 */
#include "classify.hpp"

void zs_driverMonitor::initNet() {

	resetAxiBus();
	readNetwork("/home/root/network_face"); // load network file
	loadFCParams(); // load fully connected weights
	//initializeInternalVariables();
	//launchThread();

}

void zs_driverMonitor::file_set() {

	/* re initialize all variable*/
	//loadFCParams();
	launchThread();
	m_activeProcessing = false;
	m_currentLayer = 0;

	runLoop();
	printf("ended processing\n");
	return;
}

void zs_driverMonitor::dma_set_(unsigned int* dma_virtual_address, int offset,
		unsigned int value) {
	dma_virtual_address[offset >> 2] = value;
}

unsigned int zs_driverMonitor::dma_get_(unsigned int* dma_virtual_address,
		int offset) {
	return (dma_virtual_address[offset >> 2]);
}

void zs_driverMonitor::resetAxiBus() {
	dma_set_(virtual_address_, S2MM_CONTROL_REGISTER_, 4);
	dma_set_(virtual_address_, MM2S_CONTROL_REGISTER_, 4);
	dma_set_(virtual_address_, S2MM_CONTROL_REGISTER_, 0);
	dma_set_(virtual_address_, MM2S_CONTROL_REGISTER_, 0);
}

bool zs_driverMonitor::waitValidAxiDataToRead_(int wordsNumber) {

	dma_set_(virtual_address_, S2MM_STATUS_REGISTER_, 2); // Clear idle
	dma_set_(virtual_address_, S2MM_STATUS_REGISTER_, 0x1000); // Clear IOC_Irq
	dma_set_(virtual_address_, S2MM_DESTINATION_ADDRESS_, dest_addr_offset_);
	dma_set_(virtual_address_, S2MM_CONTROL_REGISTER_, 0xf001);
	dma_set_(virtual_address_, S2MM_LENGTH_, TRANSLEN_ * wordsNumber);
	dma_s2mm_sync_(virtual_address_); // Wait until Idle or IOC_Irq bit is 1
}

void zs_driverMonitor::dma_mm2s_sync_(unsigned int* dma_virtual_address) {
	unsigned int mm2s_status = dma_get_(dma_virtual_address,
	MM2S_STATUS_REGISTER_);
	while (!(mm2s_status & 1 << 12) || !(mm2s_status & 1 << 1)) {
		mm2s_status = dma_get_(dma_virtual_address, MM2S_STATUS_REGISTER_);
	}
}

void zs_driverMonitor::dma_s2mm_sync_(unsigned int* dma_virtual_address) {
	unsigned int s2mm_status = dma_get_(dma_virtual_address,
	S2MM_STATUS_REGISTER_);

	while (!(s2mm_status & (1 << 12)) || !(s2mm_status & (1 << 1))) {
		s2mm_status = dma_get_(dma_virtual_address, S2MM_STATUS_REGISTER_);
	}
}

void zs_driverMonitor::dma_s2mm_sync_halted_and_notIDLE_(
		unsigned int* dma_virtual_address) {
	unsigned int s2mm_status = dma_get_(dma_virtual_address,
	S2MM_STATUS_REGISTER_);

	while ((s2mm_status & (1 << 12)) || (s2mm_status & (1 << 1))) {
		s2mm_status = dma_get_(dma_virtual_address, S2MM_STATUS_REGISTER_);
	}
}

void zs_driverMonitor::dma_s2mm_status_(unsigned int* dma_virtual_address) {
	unsigned int status = dma_get_(dma_virtual_address, S2MM_STATUS_REGISTER_);
	printf("Stream to memory-mapped status (0x%08x@0x%02x):", status,
	S2MM_STATUS_REGISTER_);
	if (status & 0x00000001)
		printf(" halted");
	else
		printf(" running");
	if (status & 0x00000002)
		printf(" idle");
	if (status & 0x00000008)
		printf(" SGIncld");
	if (status & 0x00000010)
		printf(" DMAIntErr");
	if (status & 0x00000020)
		printf(" DMASlvErr");
	if (status & 0x00000040)
		printf(" DMADecErr");
	if (status & 0x00000100)
		printf(" SGIntErr");
	if (status & 0x00000200)
		printf(" SGSlvErr");
	if (status & 0x00000400)
		printf(" SGDecErr");
	if (status & 0x00001000)
		printf(" IOC_Irq");
	if (status & 0x00002000)
		printf(" Dly_Irq");
	if (status & 0x00004000)
		printf(" Err_Irq");
	printf("\n");
}

void zs_driverMonitor::dma_mm2s_status_(unsigned int* dma_virtual_address) {
	unsigned int status = dma_get_(dma_virtual_address, MM2S_STATUS_REGISTER_);
	printf("Memory-mapped to stream status (0x%08x@0x%02x):", status,
	MM2S_STATUS_REGISTER_);
	if (status & 0x00000001)
		printf(" halted");
	else
		printf(" running");
	if (status & 0x00000002)
		printf(" idle");
	if (status & 0x00000008)
		printf(" SGIncld");
	if (status & 0x00000010)
		printf(" DMAIntErr");
	if (status & 0x00000020)
		printf(" DMASlvErr");
	if (status & 0x00000040)
		printf(" DMADecErr");
	if (status & 0x00000100)
		printf(" SGIntErr");
	if (status & 0x00000200)
		printf(" SGSlvErr");
	if (status & 0x00000400)
		printf(" SGDecErr");
	if (status & 0x00001000)
		printf(" IOC_Irq");
	if (status & 0x00002000)
		printf(" Dly_Irq");
	if (status & 0x00004000)
		printf(" Err_Irq");
	printf("\n");
}

void zs_driverMonitor::memdump_(char* virtual_address, int byte_count) {
	char *p = virtual_address;
	int offset;
	unsigned int data = 0;
	unsigned int data_low = 0;

	for (offset = 0; offset < byte_count; offset++) {
		data |= (p[offset] & 0xFF) << ((offset % 4) * 8);
		if (offset % 8 == 7) {
			printf("0x%08x%08x\n", data, data_low);
			data = 0;
			data_low = 0;
		} else {
			if (offset % 4 == 3) {
				data_low = data;
				data = 0;
			}
		}
	}
}

void zs_driverMonitor::memdump_checking_(char* virtual_address,
		int byte_count) {
	char *p = virtual_address;
	int offset;
	unsigned int data = 0;
	unsigned int data_low = 0;
	unsigned int data_low_bkp = 0;

	for (offset = 0; offset < byte_count; offset++) {
		data |= (p[offset] & 0xFF) << ((offset % 4) * 8);
		if (offset % 8 == 7) {
			printf("0x%08x%08x\n", data, data_low);
			if (data != 3 || data_low != data_low_bkp) {
				//resetAxiBus();
				printf(
						"Error in the sequence. is expected: high --> 00000003, low --> %d. Received: high --> %d, low --> %d",
						data_low_bkp, data, data_low);
			} else if (data_low == 0x3f) {
				data_low_bkp = 0;
			} else {
				data_low_bkp += 0x100;
			}
			data = 0;
			data_low = 0;
		} else {
			if (offset % 4 == 3) {
				data_low = data;
				data = 0;
			}
		}
	}
}

char * zs_driverMonitor::file_get() {
	return (file_i);
}

void zs_driverMonitor::launchThread() {

	// high priority
	/*	pthread_attr_t attr;
	 struct sched_param param;

	 pthread_attr_init(&attr);
	 pthread_attr_getschedparam(&attr, &param);
	 (param.sched_priority)++;
	 pthread_attr_setschedparam(&attr, &param); &attr*/

	if (pthread_create(&m_readThread, NULL, readThreadRoutine, (void *) this)
			!= 0) {
		printf("+++++ ERROR : while creating read thread.. exiting\n");
		exit(1);
	}
}

void zs_driverMonitor::loadFCParams() {
	load_single_FC_layer("ip1_params", m_ip1_params,
			sizeof(m_ip1_params) / sizeof(m_ip1_params[0]));
	load_single_FC_layer("ip2_params", m_ip2_params,
			sizeof(m_ip2_params) / sizeof(m_ip2_params[0]));

	dumpFCLayer("ip1_params_dumped", m_ip1_params,
			sizeof(m_ip1_params) / sizeof(m_ip1_params[0]));
	dumpFCLayer("ip2_params_dumped", m_ip2_params,
			sizeof(m_ip2_params) / sizeof(m_ip2_params[0]));
}

void zs_driverMonitor::load_single_FC_layer(const char *fileName,
		int * ip_params, unsigned int paramSize) {
	FILE *fp = fopen(fileName, "r");
	for (unsigned int i = 0; i < paramSize; ++i)
		safe_fscanf(fp, "%d", &ip_params[i]);
	fclose(fp);
}

void zs_driverMonitor::evaluateFCLayers() {
	for (int i = 0; i < IP1_OP_SIZE; ++i) {
		m_fc1_output[i] = 0;
		for (int j = 0; j < m_nchIn; ++j) {
			for (int k = 0; k < m_hinMax; ++k) {
				for (int l = 0; l < m_imageWidth; ++l) {
					m_fc1_output[i] += m_ip1_params[i * m_nchIn * m_hinMax
							* m_imageWidth + j * m_hinMax * m_imageWidth
							+ k * m_imageWidth + l] * m_image[j][k][l];
				}

			}
		}
		m_fc1_output[i] = (m_fc1_output[i] / 256)
				+ m_ip1_params[m_nchIn * m_hinMax * m_imageWidth * IP1_OP_SIZE
						+ i];
		m_fc1_output[i] = m_fc1_output[i] > 0 ? m_fc1_output[i] : 0;
	}

	/*dumpFCLayer("fc1_out", m_fc1_output,
	 sizeof(m_fc1_output) / sizeof(m_fc1_output[0]));*/

	for (int i = 0; i < IP2_OP_SIZE; ++i) {
		m_fc2_output[i] = 0;
		for (int j = 0; j < IP1_OP_SIZE; ++j)
			m_fc2_output[i] += m_ip2_params[i * IP1_OP_SIZE + j]
					* m_fc1_output[j];

		m_fc2_output[i] = (m_fc2_output[i] / 256)
				+ m_ip2_params[IP2_OP_SIZE * IP1_OP_SIZE + i];
	}

	/*dumpFCLayer("fc2_out", m_fc2_output,
	 sizeof(m_fc2_output) / sizeof(m_fc2_output[0]));*/

}

void zs_driverMonitor::dumpFCLayer(const char *fileName, int *fc,
		unsigned int len) {
	FILE *fp = fopen(fileName, "w");
	for (unsigned int i = 0; i < len; ++i)
		fprintf(fp, "%d\n", fc[i]);
	fclose(fp);
}

int zs_driverMonitor::ipow(int base, int exp) {
	int result = 1;
	while (exp) {
		if (exp & 1)
			result *= base;
		exp >>= 1;
		base *= base;
	}
	return (result);
}

int zs_driverMonitor::setCurrentLayer(unsigned int layerIndex) {
	if (layerIndex >= m_numLayers) {
		fprintf(stderr, "TB FINISHED. Layer %d is beyond layer number %d\n",
				layerIndex, m_numLayers);
		return (FINISHED);
	}
	m_imageWidth = m_layerParams[layerIndex].num_input_column;
	m_hinMax = m_layerParams[layerIndex].num_input_rows;
	m_nchIn = m_layerParams[layerIndex].num_input_channels;
	m_nchOut = m_nchOut_pseudo = m_layerParams[layerIndex].num_output_channels;
	m_hk = m_wk = m_layerParams[layerIndex].kernel_size;
	m_imageCompressionEnabled =
			m_layerParams[layerIndex].image_compression_enabled;
	m_reluEnabled = m_layerParams[layerIndex].relu_enabled;
	m_poolingEnabled = m_layerParams[layerIndex].pooling_enabled;
	m_encodingEnabled = m_reluEnabled;

	int numKernelsPerChannel = m_hk * m_wk * m_nchIn;
	int requiredMacsPerChannel_memory = numKernelsPerChannel / (4096) + 1;
	int requiredMacsPerChannel_numChannels;
	if (m_nchOut > 128) {
		int outputChannelBlocks = m_nchOut / 128;
		int outputChannelsPerBlock = m_nchOut
				/ (outputChannelBlocks + (m_nchOut % 128 ? 1 : 0));
		requiredMacsPerChannel_numChannels = 128 / outputChannelsPerBlock;
	} else
		requiredMacsPerChannel_numChannels = 128 / m_nchOut;

	int requiredMacsPerChannel = (
			requiredMacsPerChannel_numChannels > requiredMacsPerChannel_memory ?
					requiredMacsPerChannel_numChannels :
					requiredMacsPerChannel_memory);
	if (requiredMacsPerChannel == 1)
		requiredMacsPerChannel = 1;
	else if (requiredMacsPerChannel <= 2)
		requiredMacsPerChannel = 2;
	else if (requiredMacsPerChannel <= 4)
		requiredMacsPerChannel = 4;
	else if (requiredMacsPerChannel <= 8)
		requiredMacsPerChannel = 8;
	else {
		fprintf(stderr, "invalid macs per channel %d\n",
				requiredMacsPerChannel);
	}

	m_nchOut = m_nchOut_pseudo = 128 / requiredMacsPerChannel;
	m_numInputPasses = m_layerParams[layerIndex].num_output_channels / m_nchOut;
	m_outputChannelOffset = m_currentInputPass * m_nchOut;

	int nearestPow2 = 1;
	while (nearestPow2 < m_nchIn) {
		nearestPow2 *= 2;
	}

	if (requiredMacsPerChannel == 1) {
		m_dummyKernels = 0;
		m_contiguous_kernels = m_nchIn * m_wk * m_hk;
		m_nchIn_pseudo = m_nchIn;
		m_channel_decode_jump_mask = nearestPow2 - 1;
	} else {
		m_dummyKernels = (nearestPow2 - m_nchIn) * m_wk * m_hk;

		if (nearestPow2 <= requiredMacsPerChannel) {
			m_contiguous_kernels = m_wk * m_hk;
			m_dummyKernels += (requiredMacsPerChannel - nearestPow2) * m_wk
					* m_hk;
		} else {
			m_contiguous_kernels = (nearestPow2 / requiredMacsPerChannel) * m_wk
					* m_hk;
		}

		m_nchIn_pseudo = m_nchIn + (m_dummyKernels / (m_wk * m_hk));

		m_channel_decode_jump_mask = (m_contiguous_kernels / (m_wk * m_hk)) - 1;
	}

	fprintf(stderr,
			"num dummy kernels %d, pseudo input channel number %d, num input passes %d, output channel offset %d\n",
			m_dummyKernels, m_nchIn_pseudo, m_numInputPasses,
			m_outputChannelOffset);
	m_macs_per_channel = requiredMacsPerChannel;

	m_sent_start_pulse = false;
	m_sent_image_ready = false;
	m_kernelArray = m_layerParams[layerIndex].kernels;
	m_biases = m_layerParams[layerIndex].biases;

	//fprintf(stderr, "image pointer is %p\n", m_layerParams[layerIndex].image);
	//fprintf(stderr, "first pixel is %d\n",
	//		m_layerParams[layerIndex].image[0][0][0]);

	m_image = m_layerParams[layerIndex].image;
	m_outputImage = m_layerParams[layerIndex + 1].image;

	m_idpBuffer_pos = 0;

	m_inputLayerPadding = m_layerParams[layerIndex].padding;
	m_outputLayerPadding = m_layerParams[layerIndex + 1].padding;
	dumpImage();
	dumpKernels();

}

void zs_driverMonitor::readNetwork(const char *fileName) {
	FILE *fp = fopen(fileName, "r");
	if (!fp) {
		fprintf(stderr, "can not open network file");
		exit(1);
	}
	safe_fscanf(fp, "%d", &m_numLayers);
	if (m_layerParams) {
		fprintf(stderr, "readNetwork can only be called once\n");
		exit(1);
	}
	m_layerParams = new t_layerParams[m_numLayers + 1];

	//    m_layerFinisheTimes = new int[m_numLayers+1];
	//    memset(m_layerFinishTimes,0,sizeof(int)*(m_numLayers+1));

	for (unsigned int i = 0; i < m_numLayers; i++)
		readLayer(fp, m_layerParams[i], i == 0);

	//initialize the final layer (not really a layer, just a storage for the top layer output)
	int divisor = (m_layerParams[m_numLayers - 1].pooling_enabled ? 2 : 1);
	int last_kernel_size = m_layerParams[m_numLayers - 1].kernel_size;
	memset(&m_layerParams[m_numLayers], 0, sizeof(t_layerParams));

	m_layerParams[m_numLayers].num_input_channels = m_layerParams[m_numLayers
			- 1].num_output_channels;
	m_layerParams[m_numLayers].num_input_column =
			(m_layerParams[m_numLayers - 1].num_input_column - last_kernel_size
					+ 1 + 2 * m_layerParams[m_numLayers - 1].padding) / divisor;
	m_layerParams[m_numLayers].num_input_rows =
			(m_layerParams[m_numLayers - 1].num_input_rows - last_kernel_size
					+ 1 + 2 * m_layerParams[m_numLayers - 1].padding) / divisor;
	initializePixelArray(m_layerParams[m_numLayers]);

	error_counter = 0;
	error_on_SMs = 0;
}

void zs_driverMonitor::checkMarker(FILE *fp, const char * marker) {
	//fprintf(stderr, "checking marker %s\n", marker);
	char line[256];
	safe_fscanf(fp, "%s", line);
	if (!strstr(line, marker)) {
		fprintf(stderr, "failed to find marker %s\n", marker);
		exit(1);
	}
}

void zs_driverMonitor::readLayer(FILE *fp, t_layerParams &layerParam,
bool firstLayer) {
	unsigned int * paramsAsArray = ((unsigned int *) (&layerParam));

	unsigned int numParams = 9;

	for (unsigned int i = 0; i < numParams; ++i) {
		unsigned int temp;
		safe_fscanf(fp, "%u", &temp);
		paramsAsArray[i] = temp;
	}

	//    layerParam.num_input_rows += layerParam.padding*2;
	//    layerParam.num_input_column += layerParam.padding*2;

	//fprintf(stderr, "layer params: %u:%u:%u\n", layerParam.num_input_channels,
	//		layerParam.num_output_channels, layerParam.kernel_size);

	initializeKernelArray(layerParam);
	initializeBiasArray(layerParam);
	initializePixelArray(layerParam);

	checkMarker(fp, "#KERNELS#");
	readKernels(fp, layerParam);

	checkMarker(fp, "#BIASES#");
	readBiases(fp, layerParam);

	if (firstLayer) {
		checkMarker(fp, "#PIXELS#");
		readPixels(fp, layerParam);
	}
}

void zs_driverMonitor::initializeInternalVariables() {
	printf("internal init\n");

	current_control = current_write = current_read = 0;
	m_currentInitStep = 0;

	//s2mm_wait_counter = 0;

	m_pixelArrayWritePos = m_nPixelsArray = 0;

	memset(m_oldValuesTop, 0, sizeof(int) * 8);
	memset(m_oldValuesBot, 0, sizeof(int) * 8);
	memset(m_idp_decodePosition, 0, sizeof(unsigned int) * 8);
	memset(m_mac_stripe_index, 0, sizeof(unsigned int) * 8);

	for (unsigned int i = 0; i < 8; ++i)
		m_idp_positionConsideredOnce[i] = false;

	m_log2_macs_per_channel = 0;
	unsigned int temp = m_macs_per_channel;
	while (temp >>= 1)
		m_log2_macs_per_channel++;

	m_currentDecodeIndex = -16;
	m_currentDecodeSM = 0;

	m_firstAxiWrite = true;

	m_currentPixelPosition = m_numPixelsReady = 0;
	m_idpBuffer_pos = 0;

	m_kernelWritePos = m_imageWritePos = m_currentOutputChannel =
			m_currentOutputYPos = m_currentOutputXPos = 0;
	m_currentInputChannel = m_currentInputYPos = m_currentInputXPos =
			m_wroteImageDone = m_currentInputRowShift = m_biasWritePos = 0;

	m_completedKernelWrite = m_completedImageWrite = m_completedBiasWrite =
			m_gotAllPixels = m_completedConfigWrite = false;

	for (unsigned int i = 0; i < NUM_MAC_BLOCKS; ++i) {
		m_currentOutputCol[i] = 0;
		m_currentOutputRow[i] = 0;
	}

	m_anchorChannel = new int[m_macs_per_channel];
	m_anchorColumn = new int[m_macs_per_channel];
	m_anchorRow = new int[m_macs_per_channel];
	m_anchorShift = new int[m_macs_per_channel];
	m_previousPixelIndex = new int[m_macs_per_channel];
	m_activePatch = new int ***[m_macs_per_channel];

	for (unsigned int p = 0; p < m_macs_per_channel; ++p) {
		m_anchorChannel[p] = m_anchorColumn[p] = m_anchorRow[p] =
				m_anchorShift[p] = 0;
		m_previousPixelIndex[p] = -1;
		m_activePatch[p] = new int**[m_nchOut];

		for (int i = 0; i < m_nchOut; ++i) {
			m_activePatch[p][i] = new int *[m_hinMax];

			for (int j = 0; j < m_hinMax; ++j) {
				m_activePatch[p][i][j] = new int[m_imageWidth];

				for (int k = 0; k < m_imageWidth; ++k) {

					if (p == 0)
						m_activePatch[p][i][j][k] = m_biases[i] * 256;
					else
						m_activePatch[p][i][j][k] = 0;
				}

			}
		}
	}

	m_processedPixels = new int **[m_nchIn];

	for (int i = 0; i < m_nchIn; ++i) {
		m_processedPixels[i] = new int *[m_hinMax + m_inputLayerPadding * 2];
		for (int j = 0; j < m_hinMax + m_inputLayerPadding * 2; ++j) {
			m_processedPixels[i][j] = new int[m_imageWidth
					+ m_inputLayerPadding * 2];
			for (int k = 0; k < m_imageWidth + m_inputLayerPadding * 2; ++k) {
				m_processedPixels[i][j][k] = 0;

			}

		}

	}
}

void zs_driverMonitor::readKernels(FILE *fp, t_layerParams &layerParam) {
	int **** &kernelArray = layerParam.kernels;
	unsigned int kernel_size = layerParam.kernel_size;
	unsigned int num_input_channels = layerParam.num_input_channels;
	unsigned int num_output_channels = layerParam.num_output_channels;

	for (unsigned int i = 0; i < num_output_channels; ++i) {
		for (unsigned int j = 0; j < num_input_channels; ++j) {
			for (unsigned int k = 0; k < kernel_size; ++k) {
				for (unsigned int l = 0; l < kernel_size; ++l) {
					safe_fscanf(fp, "%d", &kernelArray[i][j][k][l]);
				}

			}
		}
	}
}

void zs_driverMonitor::readBiases(FILE *fp, t_layerParams & layerParam) {
	int * &biases = layerParam.biases;
	unsigned int num_output_channels = layerParam.num_output_channels;

	for (unsigned int i = 0; i < num_output_channels; ++i) {
		safe_fscanf(fp, "%d", &biases[i]);
	}

}

void zs_driverMonitor::readPixels(FILE *fp, t_layerParams & layerParam) {
	int *** &image = layerParam.image;
	unsigned int num_channels = layerParam.num_input_channels;
	unsigned int height = layerParam.num_input_rows;
	unsigned int width = layerParam.num_input_column;
	unsigned int padding = 0;    //layerParam.padding;

	for (unsigned int i = 0; i < num_channels; ++i)
		for (unsigned int j = 0; j < height - padding * 2; ++j)
			for (unsigned int k = 0; k < width - padding * 2; ++k) {
				safe_fscanf(fp, "%i", &image[i][j + padding][k + padding]);
			}
}

void zs_driverMonitor::initializePixelArray(t_layerParams & layerParam) {
	int *** &image = layerParam.image;
	unsigned int num_channels = layerParam.num_input_channels;
	unsigned int height = layerParam.num_input_rows;
	unsigned int width = layerParam.num_input_column;

	//fprintf(stderr, "initializing pixel array %d:%d:%d\n", num_channels, height,
	//		width);

	image = new int **[num_channels];
	for (unsigned int i = 0; i < num_channels; ++i) {
		image[i] = new int *[height];
		for (unsigned int j = 0; j < height; ++j) {
			image[i][j] = new int[width];
			for (unsigned int k = 0; k < width; ++k) {
				image[i][j][k] = 0;
			}

		}
	}
}

void zs_driverMonitor::initializeBiasArray(t_layerParams & layerParam) {
	int * &biases = layerParam.biases;
	unsigned int num_output_channels = layerParam.num_output_channels;
	//fprintf(stderr, "initializing bias array %d\n", num_output_channels);

	biases = new int[num_output_channels];
	for (unsigned int i = 0; i < num_output_channels; ++i)
		biases[i] = 0;
}

void zs_driverMonitor::initializeKernelArray(t_layerParams & layerParam) {
	int **** &kernelArray = layerParam.kernels;
	unsigned int kernel_size = layerParam.kernel_size;
	unsigned int num_input_channels = layerParam.num_input_channels;
	unsigned int num_output_channels = layerParam.num_output_channels;
	//fprintf(stderr, "initializing kernel array %d:%d:%d:%d\n",
	//		num_output_channels, num_input_channels, kernel_size, kernel_size);

	kernelArray = new int ***[num_output_channels];

	for (unsigned int i = 0; i < num_output_channels; ++i) {
		kernelArray[i] = new int **[num_input_channels];
		for (unsigned int j = 0; j < num_input_channels; ++j) {
			kernelArray[i][j] = new int*[kernel_size];
			for (unsigned int k = 0; k < kernel_size; ++k) {
				kernelArray[i][j][k] = new int[kernel_size];
				for (unsigned int l = 0; l < kernel_size; ++l) {
					kernelArray[i][j][k][l] = 0;
				}

			}
		}
	}
}

void zs_driverMonitor::initializeConfigArray() {
	m_initConfig[0] = m_imageCompressionEnabled; //config_image_compression_enabled,
	m_initConfig[1] = (128 / (16 * m_macs_per_channel)) - 1; //config_pre_sm_counter_max,
	m_initConfig[2] = m_wk; //config_kernel_size,
	m_initConfig[3] = m_nchIn; //config_num_input_channels,
	m_initConfig[4] = m_imageWidth; //config_num_input_column,
	m_initConfig[5] = m_hinMax; //config_num_input_rows,
	m_initConfig[6] = m_nchOut; //config_num_output_channels,
	m_initConfig[7] = m_poolingEnabled; //config_pooling_enabled,
	m_initConfig[8] = m_reluEnabled; //config_relu_enabled,
	m_initConfig[9] = m_contiguous_kernels; //config_contiguous_kernels
	m_initConfig[10] = m_macs_per_channel - 1; //config_num_macs_per_channel,
	m_initConfig[11] = m_channel_decode_jump_mask; //config_input_channel_decode_jump_mask,
	m_initConfig[12] = 0; // config_kernel_memory_write_complete_pulse,
	m_initConfig[13] = 0; //config_kernel_memory_resetn_pulse
	m_initConfig[14] = 0; //config_input_image_done
}

int zs_driverMonitor::getGroundTruthPixel(unsigned int outputChannel, int xPos,
		int yPos, int & fullResResult, bool debug) {
	Assert(outputChannel < m_layerParams[m_currentLayer].num_output_channels,
			"channel index for output pixel out of range");
	Assert(xPos < m_imageWidth, "xPos for output pixel out of range");
	Assert(yPos < m_hinMax, "yPos for output pixel out of range");

	//    outputChannel *= (m_nchOut_pseudo/m_nchOut);
	//    fprintf(stderr,"start of ggtp\n");

	fullResResult = 0;
	int result = 0;
	xPos -= m_inputLayerPadding;
	yPos -= m_inputLayerPadding;

	for (int k = 0; k < m_nchIn; ++k) {
		int mulResult = 0;
		for (int i = yPos; i < yPos + m_hk; ++i)
			for (int j = xPos; j < xPos + m_wk; ++j) {
				if (i >= 0 && j >= 0 && i < m_hinMax && j < m_imageWidth) {
					int pixel = m_image[k][i][j];
					// if(k==0)
					//   fprintf(m_log,"CONV MULT - IMAGE: %d - KERNEL: %d \n", m_image[k][i][j],m_kernelArray[outputChannel][k][i-yPos][j-xPos]);
					mulResult +=
							pixel
									* m_kernelArray[outputChannel][k][i - yPos][j
											- xPos];
					//  if(debug)
					//fprintf(stderr,"pixel %d , kernel %d runningSum:%d , %d\n",pixel,m_kernelArray[outputChannel][k][i-yPos][j-xPos],mulResult,fullResResult);
				}
			}

		fullResResult += mulResult;
		//  std::cout << "fullResResult         :" << fullResResult << std::endl;
		//  std::cout << "fullResResult shifted :" <<((fullResResult>>8)<<8) << std::endl;

		mulResult =
				(truncateInt(mulResult >> m_nFracBits, 12)
						| (mulResult < 0 ?
								(~0 << (sizeof(int) * 8 - m_nFracBits)) : 0)); //sign extension to fill the leftmost m_nFracBits
		result = result + mulResult;
		// std::cout << "result                :" << result << std::endl;

		result = truncateInt(result + mulResult, 12);
	}
	//if (debug)
	// fprintf(stderr, "bias %d , sum before bias %d\n",
	// m_biases[outputChannel * (m_nchOut_pseudo / m_nchOut)] << 8,
	// fullResResult);

	fullResResult += (m_biases[outputChannel * (m_nchOut_pseudo / m_nchOut)]
			<< 8);

	return (fullResResult);

	/*int result_trunc;
	 if (result > 0) {
	 result_trunc = (result >> m_nFracBits);
	 } else {
	 result_trunc = (result >> m_nFracBits
	 | (result < 0 ? (~0 << (sizeof(int) * 8 - m_nFracBits)) : 0));
	 int result_shift = result << (sizeof(int) * 8 - m_nFracBits);
	 //result_shift = result_shift >> (sizeof(int)*8 - m_nFracBits);

	 if (result_shift != 0) {
	 result_trunc = result_trunc + 1; //Added missing -1 for 2 complement
	 }
	 }

	 result_trunc = truncateInt(result_trunc,16);
	 result_trunc <<= 8;
	 std::cout << "result_trunc           :" << result_trunc << std::endl;

	 return result_trunc;*/
} ///// END int getGroundTruthPixel(unsigned int outputChannel,int xPos,int yPos,int & fullResResult,bool debug = false)

void zs_driverMonitor::sendConfigData(CONFIG_TYPE config_type, int data) {
	input_sigs->s_input_bus_valid = 1;
	input_sigs->s_input_bus_config_reg_addr[0] = ((int) config_type);
	input_sigs->s_input_bus_data[0] = data;
	input_sigs->s_input_bus_type = 3;
	writeToAxi();
}

bool zs_driverMonitor::initializationLoop() {

	if (m_currentInitStep <= config_kernel_memory_resetn_pulse) {
		sendConfigData((CONFIG_TYPE) m_currentInitStep,
				m_initConfig[m_currentInitStep]);
		//fprintf(stderr, "initi step %d\n", m_currentInitStep);
		++m_currentInitStep;
		return (false);
	} else {
		if (m_currentInitStep == config_kernel_memory_resetn_pulse + 1) {
			sendConfigData(config_padding_set, m_inputLayerPadding);
			//fprintf(stderr, "initi step padding %d\n", m_currentInitStep);
			++m_currentInitStep;
			return (false);
		}
		return (true);
	}

}

void zs_driverMonitor::writeBiasValue(int biasValue, int biasAddress) {
	input_sigs->s_input_bus_valid = 1;
	input_sigs->s_input_bus_data[0] = biasValue;
	input_sigs->s_input_bus_type = 0;
	input_sigs->s_input_bus_config_reg_addr[0] = biasAddress;
	writeToAxi();

}

void zs_driverMonitor::writeKernelValue(int kernelValue[2], int validMask) {
	input_sigs->s_input_bus_valid = validMask;
	input_sigs->s_input_bus_data[0] = kernelValue[0];
	input_sigs->s_input_bus_data[1] = kernelValue[1];
	input_sigs->s_input_bus_type = 2;
	writeToAxi();
}

void zs_driverMonitor::writePixels(int pos0, int pos1, bool pos1_valid,
		int instruction[2]) {
	input_sigs->s_input_bus_valid = (pos1_valid ? 3 : 1);
	input_sigs->s_input_bus_data[0] = pos0;
	input_sigs->s_input_bus_data[1] = (pos1_valid ? pos1 : 0);
	input_sigs->s_input_bus_type = 1;
	input_sigs->s_input_bus_config_reg_addr[0] = instruction[0];
	input_sigs->s_input_bus_config_reg_addr[1] = instruction[1];
	writeToAxi();
}

uint16_t zs_driverMonitor::int_to_short(int data) {

	uint16_t newData;

	newData = (uint16_t) data;
	if (data < 0) {
		newData = (uint16_t) ~(data - 1);
		newData = (~newData) + 1;
	}
	return (newData);

}

void zs_driverMonitor::writeToAxi() {

	unsigned int axiWord[2];
	memset(axiWord, 0, sizeof(unsigned int) * 2);

	axiWord[0] =
			((unsigned int) int_to_short(input_sigs->s_input_bus_data[0])
					| (unsigned int) int_to_short(
							input_sigs->s_input_bus_data[1]) << 16);

	axiWord[1] = (unsigned int) int_to_short(input_sigs->s_input_bus_type)
			| ((unsigned int) int_to_short(input_sigs->s_input_bus_valid) << 2)
			| ((unsigned int) int_to_short(
					input_sigs->s_input_bus_config_reg_addr[0]) << 4)
			| ((unsigned int) int_to_short(
					input_sigs->s_input_bus_config_reg_addr[1]) << 11);

	//fprintf(m_axiDebug,"lo %d, hi %d\n",axiWord[0],axiWord[1]);
	/*m_axiDebug = fopen("axiDebug","a");
	 fprintf(m_axiDebug,"%08x%08x\n",axiWord[1],axiWord[0]);
	 fclose(m_axiDebug);*/

	/*printf("virtual source address  %p\n", virtual_source_address_);
	 printf("virtual address  %p\n", virtual_address_);
	 printf("virtual destination address  %p\n", virtual_destination_address_);*/

	if (m_firstAxiWrite) {
		//printf("first axi write setting \n");
		axiWord[1] |= (1 << 18);
		axiWord[1] |= (MAX_BURST << 19); //(m_maxOutPixelsNum << 19);
		m_firstAxiWrite = false;
	}

	if (current_write >= MEM_SIZE - 2) {
		printf("exceeded memory mapped area for AXI write, committing first\n");
		axiWriteCommit();
	}

	virtual_source_address_[current_write] = axiWord[0];
	virtual_source_address_[current_write + 1] = axiWord[1];
	current_write += 2;

}

void zs_driverMonitor::axiWriteCommit() {

	if (current_write == 0)
		return;
	//fprintf(stderr,"committing axi writes\n");

	unsigned int size_int = sizeof(int);

	unsigned int padding = (MAX_BURST * (TRANSLEN_ / size_int))
			- (current_write % (MAX_BURST * (TRANSLEN_ / size_int)));
	int loPadding = virtual_source_address_[current_write - 2];
	int hiPadding = virtual_source_address_[current_write - 1] & (~(12)); //set valid bits to zero

	for (unsigned int i = 0; i < padding / 2; ++i) {
		virtual_source_address_[current_write] = loPadding;
		virtual_source_address_[current_write + 1] = hiPadding;
		current_write += 2;
	}

	for (unsigned int i = 0; i < current_write;
			i += MAX_BURST * (TRANSLEN_ / size_int)) //we are writing MAX_BURST*TRANSLEN bytes per iteration
					{
		unsigned int startPos = i * size_int; //start position for transfer in bytes

		unsigned int burst_size = min(MAX_BURST * TRANSLEN_,
				(current_write - i) * size_int);

		//printf("writing %d starting from %d\n",burst_size,startPos);

		if (burst_size != MAX_BURST * TRANSLEN_)
			fprintf(stderr, "error in padding\n");

		writeAxiCommit_(MAX_BURST, startPos);

	}

	current_write = 0;

	//printf("axiwrite commit\n");
	//printf("finished committing axi writes\n");

}

int zs_driverMonitor::writeAxiCommit_(int wordsNumber, unsigned int startPos) {

	//printf("%p \n", (void*)virtual_address_);

	int numbytes = 0;

	if (wordsNumber > 0 || wordsNumber <= 64) {

		dma_s2mm_sync_halted_and_notIDLE_(virtual_address_);

		// Set destination and source addresses
		//dma_set(virtual_address, S2MM_DESTINATION_ADDRESS, dest_addr_offset);
		dma_set_(virtual_address_, MM2S_START_ADDRESS_,
		src_addr_offset_ + startPos);

		// Enable interruptions and start S2MM and MM2S
		//dma_set(virtual_address, S2MM_CONTROL_REGISTER, 0xf001);
		dma_set_(virtual_address_, MM2S_CONTROL_REGISTER_, 0xf001);

		// Set tranference length for S2MM and MM2S. S2MM must be set before MM2S. In this point the tranferece starts
		//dma_set(virtual_address, S2MM_LENGTH, TRANSLEN*wordsNumber);
		dma_set_(virtual_address_, MM2S_LENGTH_, TRANSLEN_ * wordsNumber);

		dma_mm2s_sync_(virtual_address_);

		dma_set_(virtual_address_, MM2S_CONTROL_REGISTER_, 0); // Stop MM2S
		//dma_set(virtual_address, MM2S_STATUS_REGISTER, 2); // Clear idle
		dma_set_(virtual_address_, MM2S_STATUS_REGISTER_, 0x1000); // Clear IOC_Irq

		numbytes = TRANSLEN_ * wordsNumber;
	}

	return (numbytes);
}

unsigned int zs_driverMonitor::reverseInt(unsigned int src) {
	unsigned int res = 0;
	res |= (src & (255 << 24)) >> 24;
	res |= (src & (255 << 16)) >> 8;
	res |= (src & (255 << 8)) << 8;
	res |= (src & 255) << 24;
	return (res);
}

void zs_driverMonitor::stopS2MM_() {
	dma_set_(virtual_address_, S2MM_CONTROL_REGISTER_, 0); // Stop S2MM
	dma_set_(virtual_address_, S2MM_STATUS_REGISTER_, 2); // Clear idle
	dma_set_(virtual_address_, S2MM_STATUS_REGISTER_, 0x1000); // Clear IOC_Irq
}

void zs_driverMonitor::readFromAxi() {

	//printf("start reading \n");
	waitValidAxiDataToRead_(MAX_BURST);
	//printf("reading from AXI...\n");
	unsigned int size_int = sizeof(int);
	for (unsigned int i = 0; i < MAX_BURST * (TRANSLEN_ / size_int); i += 2) {
		fprintf(m_readAxiFile,
				"Reading from axi: high int %08x, low int %08x\n",
				virtual_destination_address_[i + 1],
				virtual_destination_address_[i]);
		m_readWordsAxiFile = fopen("/home/root/readWordsAxiFile", "a");
		fprintf(m_readWordsAxiFile, "%08x%08x\n",
				virtual_destination_address_[i + 1],
				virtual_destination_address_[i]);
		fclose(m_readWordsAxiFile);
		output_sigs->s_output_pixel_stream = virtual_destination_address_[i];
		output_sigs->s_output_pixel_stream_valid =
				(virtual_destination_address_[i + 1] & 3);

		if (output_sigs->s_output_pixel_stream == 0
				&& output_sigs->s_output_pixel_stream_valid == 0
				&& m_currentLayer == m_numLayers) {
			fclose(m_readAxiFile);
			stopS2MM_();
			pthread_exit(NULL);
		}
		phase1_step();

	}
}

bool zs_driverMonitor::matchToPatch(int output_pixel, int pixel_ch,
		int pixel_xpos, int pixel_ypos, unsigned int mac_index) {
	//    fprintf(stderr,"in match to patch %d at ch:col:row:mac_index %d:%d:%d %d\n",output_pixel,pixel_ch,pixel_xpos,pixel_ypos,mac_index);
	int currentPatchValue =
			m_activePatch[mac_index][pixel_ch][pixel_ypos][pixel_xpos];
	//fprintf(m_log, "***********current MAC index %d\n", mac_index);

	if (output_pixel == currentPatchValue) {
		//fprintf(m_log, "Pixel already matched at %d at ch:col:row %d:%d:%d \n",
		//		output_pixel, pixel_ch, pixel_xpos, pixel_ypos);
		//fflush(m_log);
		return (true);
	}

	unsigned int startPos = m_idp_decodePosition[mac_index];
	unsigned int activePos = startPos;
	int input_pixel, channel_start, row_start, column_start, shift_start;
	for (; activePos < m_idpBuffer_pos; ++activePos) {

		if ((int) mac_index == (int) m_idp_mac_index[activePos]) {
			input_pixel = m_idp_pixels[activePos];
			channel_start = m_idp_channel[activePos];
			row_start = m_idp_row[activePos];
			column_start = m_idp_column[activePos];
			shift_start = m_idp_shift[activePos] + m_inputLayerPadding;
			break;
		}
	}

	if (activePos == m_idpBuffer_pos) {
		//fprintf(m_log,
		//		"Failed to find a candidate input pixel for for output pixel %d at ch:col:row %d:%d:%d . num pixels in buffer %d\n",
		//		output_pixel, pixel_ch, pixel_xpos, pixel_ypos,
		//		m_idpBuffer_pos);
		//fflush(m_log);
		return (false);
	}

	int actualYPos = row_start + shift_start;
	bool consideredOnce =
			(startPos == activePos) ?
					(m_idp_positionConsideredOnce[mac_index]) : false;

	if (actualYPos < pixel_ypos + m_hk && actualYPos >= pixel_ypos)
		if (column_start < pixel_xpos + m_wk && column_start >= pixel_xpos) {
			//fprintf(m_log, "currentPixel pixel %d at ch:col:row %d:%d:%d . \n",
			//		output_pixel, pixel_ch, pixel_xpos, pixel_ypos);
			//fprintf(m_log, "current indices ch:col:row:shift %d:%d:%d:%d\n",
			//		channel_start, column_start, row_start, shift_start);

			//fprintf(m_log,
			//		"current kernel (out_ch:in_ch:ypos:xpos %d:%d:%d:%d) and input pixel and patch %d %d %d. input pixel buffer position %d and considered once %d \n",
			//		pixel_ch, channel_start, actualYPos - pixel_ypos,
			//		column_start - pixel_xpos,
			//		m_kernelArray[pixel_ch + m_outputChannelOffset][channel_start][actualYPos
			//				- pixel_ypos][column_start - pixel_xpos],
			//		input_pixel, currentPatchValue, activePos, consideredOnce);

			int candidate =
					currentPatchValue
							+ input_pixel
									* m_kernelArray[pixel_ch
											+ m_outputChannelOffset][channel_start][actualYPos
											- pixel_ypos][column_start
											- pixel_xpos];

			if (candidate == output_pixel) {
				m_activePatch[mac_index][pixel_ch][pixel_ypos][pixel_xpos] =
						output_pixel;
				/*fprintf(m_log, "setting patch %d:%d:%d to %d\n", pixel_ch,
				 pixel_ypos, pixel_xpos, output_pixel);*/
				m_processedPixels[channel_start][actualYPos][column_start]++;
				unsigned int currentPixelIndex = (shift_start + row_start)
						* m_imageWidth * m_nchIn + column_start * m_nchIn
						+ channel_start;
				/*fprintf(m_log,
				 "debug currentPixelIndex: %d, m_previousPixelIndex : %d, currentPatchValue : %d, output_pixel : %d\n",
				 currentPixelIndex, m_previousPixelIndex[mac_index],
				 currentPatchValue, output_pixel);*/
				if ((int) currentPixelIndex
						!= (int) m_previousPixelIndex[mac_index]) {
					m_previousPixelIndex[mac_index] = currentPixelIndex;
					if (currentPatchValue != output_pixel) {
						for (int j = 0; j < m_hk; ++j) {
							for (int k = 0; k < m_wk; ++k) {
								if (actualYPos >= j)
									if (column_start >= k)
										if (actualYPos - j < shift_start + 2)
											if (pixel_xpos
													!= column_start - k) {
												/*fprintf(m_log,
												 "setting extended patch %d:%d:%d  %d",
												 pixel_ch,
												 actualYPos - j,
												 column_start - k,
												 m_activePatch[mac_index][pixel_ch][actualYPos
												 - j][column_start
												 - k]);*/
												m_activePatch[mac_index][pixel_ch][actualYPos
														- j][column_start - k] +=
														input_pixel
																* m_kernelArray[pixel_ch
																		+ m_outputChannelOffset][channel_start][j][k];
												/*fprintf(m_log,
												 "  ---->  %d (%d*%d)\n",
												 m_activePatch[mac_index][pixel_ch][actualYPos
												 - j][column_start
												 - k],
												 input_pixel,
												 m_kernelArray[pixel_ch
												 + m_outputChannelOffset][channel_start][j][k]);*/

											}
							}
						}
					}

				}
				if (consideredOnce || row_start == 0 || row_start == m_hk) {
					m_idp_decodePosition[mac_index] = activePos + 1;
					m_idp_positionConsideredOnce[mac_index] = false;
				} else {
					m_idp_decodePosition[mac_index] = activePos;
					m_idp_positionConsideredOnce[mac_index] = true;
				}

				//fflush(m_log);
				return true;

			}
		}

	/*fprintf(m_log,
	 "candidate pixel failed to match at buffer position %d for output pixel %d at ch:col:row %d:%d:%d . num pixels in buffer %d\n",
	 activePos, output_pixel, pixel_ch, pixel_xpos, pixel_ypos,
	 m_idpBuffer_pos);
	 fflush(m_log);*/
	return false;
}

void zs_driverMonitor::generateSMandPixels(unsigned int outputHeight,
		unsigned int outputWidth) {
	for (unsigned int SMidx = 0; SMidx < 16; ++SMidx) { //calculate the 16 pixels and generate SM

		bottomRow = false;
		if (!m_poolingEnabled) {
			if ((tempPos / m_nchOut) % 2) //bottom Row
					{
				tempPos -= m_nchOut;
				bottomRow = true;
			}

			shift = tempPos % m_nchOut;
			tempPos = (tempPos - shift) / 2 + shift;
		}

		chIdx = tempPos % m_nchOut + m_outputChannelOffset;
		tempPos /= m_nchOut;
		xPos = tempPos % outputWidth;
		tempPos /= outputWidth;
		yPos = tempPos * (m_poolingEnabled ? 1 : 2) + (bottomRow ? 1 : 0);

		maxPixel = -2147000000;

		if (m_poolingEnabled) {
			for (int i = 0; i < 2; ++i)
				for (int j = 0; j < 2; ++j) {
					//			  fprintf(stderr," %d:%d:%d->",chIdx,xPos*2+i,yPos*2+j);
					getGroundTruthPixel(chIdx, xPos * 2 + i, yPos * 2 + j,
							outputGroundTruthPixel);
					//			  fprintf(stderr," %d ,",outputGroundTruthPixel);
					maxPixel =
							maxPixel > outputGroundTruthPixel ?
									maxPixel : outputGroundTruthPixel;
					//   std::cout << "maxPixel in calc pooling/relu "<< maxPixel <<std::endl;

				}

		} else
			// when no pooling
			getGroundTruthPixel(chIdx, xPos, yPos, maxPixel);

		if (m_reluEnabled)
			maxPixel = maxPixel > 0 ? maxPixel : 0;

		maxPixel >>= 8;

		SMpixels[SMidx] = maxPixel;

		if (maxPixel != 0)
			generatedSM |= (1 << SMidx);

		//if (tempPos > 270)
		//	std::cout << "generatedSM     " << generatedSM << std::endl;

		if ((maxPixel < 0) & (m_reluEnabled == 1))
			std::cout
					<< "******ERROR: ReLU enabled but negative pixel generated - tempPos: "
					<< tempPos << std::endl;

		tempPos = m_currentDecodeIndex + (SMidx + 1);

	}    //END calculate the 16 pixels and generate SM

} // END generateSMandPixels

void zs_driverMonitor::phase1_step() {
	if (m_gotAllPixels) {
		return;
	}

	unsigned int unpooled_outputHeight = (m_hinMax - m_hk + 1
			+ m_inputLayerPadding * 2);
	unsigned int unpooled_outputWidth = m_imageWidth - m_wk + 1
			+ m_inputLayerPadding * 2;

	unsigned int outputHeight = unpooled_outputHeight;
	unsigned int outputWidth = unpooled_outputWidth;
	if (m_poolingEnabled) {
		outputHeight /= 2;
		outputWidth /= 2;
	}

	if (output_sigs->s_output_pixel_stream_valid) {
		int out_stream = output_sigs->s_output_pixel_stream;
		/*fprintf(m_log,
		 "stream valid: %d, stream value %08x (low %d, high %d)\n",
		 output_sigs->s_output_pixel_stream_valid, out_stream,
		 out_stream & (0xFFFF), (out_stream & (0xFFFF0000)) >> 16);*/

		fprintf(m_readAxiFile,
				"stream valid: %d, stream value %08x (low %d, high %d)\n",
				output_sigs->s_output_pixel_stream_valid, out_stream,
				out_stream & (0xFFFF), (out_stream & (0xFFFF0000)) >> 16);

		for (unsigned int decode_iter = 0; decode_iter < 2; ++decode_iter) {
			int decoded_value = (
					decode_iter == 0 ?
							output_sigs->s_output_pixel_stream
									& (ipow(2, 16) - 1) :
							(output_sigs->s_output_pixel_stream >> 16));

			if ((m_currentDecodeSM == 0) & (m_encodingEnabled == 1)) // when encoding is enabled. Only when SM is all zeros, i.e. when all pixels have been read.
					{
				m_currentDecodeSM = decoded_value;
				m_currentDecodeIndex += 16; // initialized to -16, here put back to zero

				tempPos = m_currentDecodeIndex;
				generatedSM = 0;

				generateSMandPixels(outputHeight, outputWidth);

				std::bitset<16> x(generatedSM);
				std::bitset<16> y(m_currentDecodeSM);

				if (decoded_value != generatedSM) {
					//      std::cout << "\n******ERROR BETWEEN SM GENERATED AND OUT OF CHIP: generatedSM   " << x<< "   decimal (" << generatedSM <<")"  << "   decodedSM   " << y << "   decimal (" << m_currentDecodeSM <<")" << std::endl;
					//      std::cout << "tempPos   " << tempPos <<std::endl;
					error_on_SMs++;
					//      std::cout << "error_on_SMs   " << error_on_SMs <<std::endl;

					fprintf(m_readAxiFile,
							"******ERROR BETWEEN SM GENERATED AND OUT OF CHIP ******** generatedSM: (decimal %d). decodedSM: (decimal %d) \n",
							generatedSM, m_currentDecodeSM);
					fprintf(m_readAxiFile, "tempPos is: %d \n", tempPos);
					fprintf(m_readAxiFile, "error_on_SMs is: %d \n",
							error_on_SMs);

				}
				/* else
				 std::cout << "correct SM:  "<< x << "   decimal (" << generatedSM <<")" << std::endl;*/

				//// CHECK generatedSM and outputSM from chip. Correction to put to zero those pixels lower than truncation tolerance in generatedSM.
				for (unsigned int i = 0; i < 16; ++i) {

					if (((m_currentDecodeSM & (1 << i)) == 0)
							& ((generatedSM & (1 << i)) == (1 << i))) {
						fprintf(m_readAxiFile,
								"SM index of possible error: %d, generatedSM: %d, decodedSM: %d \n",
								i, generatedSM, m_currentDecodeSM);
						fprintf(m_readAxiFile,
								"Generated SM has different entry wrt output SM - SMpixels[i] is: %d \n",
								SMpixels[i]);

						// std::cout << "i:   " <<i << ",   generatedSM   " << x << ",   decodedSM   " << y << std::endl;
						// std::cout << "Generated SM has different entry wrt output SM - SMpixels[i] is:  " << SMpixels[i] <<std::endl;

						if (SMpixels[i] < PRE_TRUNCATION_TOLERANCE) {
							generatedSM &= (~(1 << i));
							std::bitset<16> x(generatedSM);
							//   std::cout << "*** CHANGED AS generatedSM PIXEL IS WITHIN THE PRE_TRUNCATION_TOLERANCE *** generatedSM   " << x << ",   decodedSM   " << y << std::endl;

							fprintf(m_readAxiFile,
									"*** CHANGED AS generatedSM PIXEL IS WITHIN THE PRE_TRUNCATION_TOLERANCE *** generatedSM: %d , decodedSM: %d \n",
									generatedSM, m_currentDecodeSM);
						} else {
							fprintf(m_readAxiFile,
									"***** REAL ERROR BETWEEN generatedSM: %d , decodedSM: %d \n",
									generatedSM, m_currentDecodeSM);
							// std::cout << "********************************** REAL ERROR BETWEEN generatedSM   " << x << ",   decodedSM   " << y << std::endl;
						}
					}

					//   if(i==15)
					//       std::cout<<"***********"<<std::endl;
				}         // end of SM correction

				m_currentDecodeSM = generatedSM;
			}

			else // if SM has any entry non zero, both for encoding enabled or disabled.
			{

				if ((m_currentDecodeSM == 0) & (m_encodingEnabled == 0)) // only when SM is all zeros, when encoding is DISABLED.
						{
					m_currentDecodeIndex += 16;
					tempPos = m_currentDecodeIndex;
					generateSMandPixels(outputHeight, outputWidth);
					m_currentDecodeSM = (ipow(2, 16) - 1);
				}

				//// from here, part common to encoding ENABLED and DISABLED.

				// assign value to outputPixel from one of the two halves of the output stream.
				int outputPixel = decoded_value;
				if (decoded_value & (1 << 15))
					outputPixel |= ((ipow(2, 16) - 1) << 16);
				else
					outputPixel &= ((ipow(2, 16) - 1));

				// which pixel is selected from the SM. If the SM is taken from the chip, the pixel is still to be generated - otherwise, if the SM is generated using the function, its pixels are stored in SMpixels[].
				unsigned int MSB_one = 0;

				while (!((1 << MSB_one) & m_currentDecodeSM))
					++MSB_one;

				m_currentDecodeSM &= ~((1 << MSB_one));

				tempPos = m_currentDecodeIndex + MSB_one;
				//printf(stderr," tempPos is %d. \n", tempPos);

				//// inserted heartbeat
				//ALL PIXELS OF THE LAYER: ,outputHeight * outputWidth * m_nchOut
				/*   unsigned int heartbeatPulse;
				 unsigned int hbIdx;
				 if(tempPos==0){
				 heartbeatPulse = ( (outputHeight * outputWidth * m_nchOut) >> 1);  // about 2 pulses per layer.
				 hbIdx=1;
				 }
				 if(tempPos>(heartbeatPulse*hbIdx) ) {
				 hbIdx++;
				 fprintf(stderr,"\n Heartbeat... (Produced %d pixels of the current layer in the current pass). \n", tempPos);
				 }*/

				// part needed to print the position of the output pixel (even if already calculated)
				bottomRow = false;

				if (!m_poolingEnabled) {
					if ((tempPos / m_nchOut) % 2) //bottom Row
							{
						tempPos -= m_nchOut;
						bottomRow = true;
					}

					shift = tempPos % m_nchOut;
					tempPos = (tempPos - shift) / 2 + shift;
				}

				// get the position of the output pixel to be generated from tempPos.
				chIdx = tempPos % m_nchOut + m_outputChannelOffset;
				tempPos /= m_nchOut;
				xPos = tempPos % outputWidth;
				tempPos /= outputWidth;
				yPos = tempPos * (m_poolingEnabled ? 1 : 2)
						+ (bottomRow ? 1 : 0);

				////// from here should be removed, as the pixels of the SM are already generated and stored in SMpixels[].
				/*
				 // outputGroundTruthPixel;
				 maxPixel = -2147000000;
				 if(m_poolingEnabled)
				 {
				 for(int i=0;i<2;++i)
				 for(int j=0;j<2;++j)
				 {
				 //			  fprintf(stderr," %d:%d:%d->",chIdx,xPos*2+i,yPos*2+j);
				 getGroundTruthPixel(chIdx,xPos*2+i,yPos*2+j,outputGroundTruthPixel);
				 //			  fprintf(stderr," %d ,",outputGroundTruthPixel);
				 maxPixel = maxPixel > outputGroundTruthPixel ? maxPixel : outputGroundTruthPixel;
				 }
				 //		    fprintf(stderr,"\n");
				 }
				 else // when no pooling
				 getGroundTruthPixel(chIdx,xPos,yPos,maxPixel);

				 if(m_reluEnabled)
				 maxPixel = maxPixel > 0 ? maxPixel : 0;

				 maxPixel >>=8;

				 ////////////// generation of pixels stops here


				 //// CHECK between pixels calculated in the function generateSMandPixels, and pixels calculated after getting the SM from the chip. - NO PIXELS FROM THE CHIP ARE EVALUATED HERE.
				 if( maxPixel != SMpixels[MSB_one])
				 {
				 std::cout << "++++++++ ERROR: maxPixel "<< std::setw(8) << maxPixel<< std::setw(8)  << " is different from SMpixels["<< MSB_one <<"]" << std::setw(16) <<SMpixels[MSB_one] <<std::endl;
				 }

				 else{std::cout << "maxPixel equals SMpixels["<< MSB_one <<"]" << std::setw(16) <<SMpixels[MSB_one] <<std::endl;}
				 //// END CHECK pixels calculated in the 2 different ways.

				 */

				/* //////////////////////////////////////// INSERTED ERRORS ////////////////////////////////////
				 if(chIdx==0 & yPos==2 & xPos==3)
				 {
				 if(outputPixel<0)
				 outputPixel-=10*PRE_TRUNCATION_TOLERANCE;
				 else
				 outputPixel+=10*PRE_TRUNCATION_TOLERANCE;
				 }
				 */

				/*      //// checking condition, when calculating the pixels from the SM taken from the chip.
				 if(abs(maxPixel - outputPixel) > PRE_TRUNCATION_TOLERANCE) {
				 fprintf(stderr,"*********************** ERROR when decoding output of PRE block, in position row:col:ch %d:%d:%d  ---  got pixel %d, expected %d\n",yPos,xPos,chIdx,outputPixel,maxPixel);
				 error_counter++;}
				 //else
				 //fprintf(m_log,"Successfully matched output of PRE block, got pixel %d at row:col:ch %d:%d:%d. Expected %d\n",outputPixel,yPos,xPos,chIdx,maxPixel);
				 */

				//// CHECK IF CALCULATED PIXELS ARE EQUAL TO PIXELS FROM CHIP
				if (abs(
						SMpixels[MSB_one]
								- outputPixel) >PRE_TRUNCATION_TOLERANCE) {
					fprintf(m_readAxiFile,
							"*********************** ERROR when decoding output of PRE block, in position row:col:ch %d:%d:%d  ---  got pixel %d, expected %d\n",
							yPos, xPos, chIdx, outputPixel, SMpixels[MSB_one]);
					std::cout << "++++++++ ERROR: outputPixel " << std::setw(8)
							<< outputPixel << std::setw(8)
							<< " is different from SMpixels[" << MSB_one << "]"
							<< std::setw(16) << SMpixels[MSB_one] << std::endl;
					error_counter++;
					fprintf(m_readAxiFile,
							"*********************** ERROR number %d\n",
							error_counter);
					//std::cout << "error_counter: " << error_counter
					//		<< std::endl;
				} else
					fprintf(m_readAxiFile,
							"Successfully matched output of PRE block: got pixel %d at row:col:ch %d,%d,%d. Expected %d \n",
							outputPixel, yPos, xPos, chIdx, SMpixels[MSB_one]);
				//std::cout << "Successfully matched output of PRE block: got pixel" << std::setw(8) << outputPixel << " at row:col:ch"<< std::setw(8) << yPos << ", " << xPos << ", " << chIdx<< std::setw(8) <<" . Expected "<< std::setw(8) << SMpixels[MSB_one]  << std::endl;
				//// end check pixels from chip.

				m_outputImage[chIdx][yPos][xPos] = outputPixel;

				if (m_currentDecodeSM == 0
						&& (m_currentDecodeIndex + 16)
								== (int) (outputHeight * outputWidth * m_nchOut)) {
					fprintf(stderr,
							"++++++++++++++++++++++++++++++++++++++++++Got all pixels %d\n",
							outputHeight * outputWidth * m_nchOut);
					fprintf(m_readAxiFile,
							"++++++++++++++++++++++++++++++++++++++++++Got all pixels %d\n",
							outputHeight * outputWidth * m_nchOut);
					m_gotAllPixels = true;
				}

			}
			if (output_sigs->s_output_pixel_stream_valid == 1)
				break;
		}
	}

}

void zs_driverMonitor::phase2_step() {
	//write the kernel registers
	input_sigs->s_output_pixel_stream_enable = 1;
	if (m_completedImageWrite)
		fprintf(m_logPixels, "DONE\n");

	if (!m_completedConfigWrite)
		m_completedConfigWrite = initializationLoop();

	else if (!m_completedBiasWrite) {
		if (!(m_biasWritePos % m_macs_per_channel))
			writeBiasValue(
					m_biases[m_biasWritePos / m_macs_per_channel
							+ m_outputChannelOffset], m_biasWritePos);
		else
			writeBiasValue(0, m_biasWritePos);

		m_biasWritePos++;
		// if(m_nchOut != m_nchOut_pseudo)
		//   {
		//     m_biasWritePos += m_nchOut_pseudo / m_nchOut;
		//     fprintf(stderr,"bias increment %d\n",m_nchOut_pseudo / m_nchOut);
		//   }
		// else
		//   m_biasWritePos += m_macs_per_channel;
		m_completedBiasWrite = (m_biasWritePos >= NUM_MAC_BLOCKS);
	}

	else if (!m_completedKernelWrite) {
		int nKernels = 0;
		int kernel[2];
		kernel[0] = kernel[1] = 0;
		for (; nKernels < 2; ++nKernels) {
			unsigned int tempPos = m_kernelWritePos;

			unsigned int xPos = tempPos % m_wk;
			tempPos /= m_wk;

			unsigned int yPos = tempPos % m_hk;
			tempPos /= m_hk;

			unsigned int srcChPos = tempPos % m_nchIn_pseudo;
			tempPos /= m_nchIn_pseudo;

			unsigned int dstChPos = tempPos + m_outputChannelOffset;

			int pseudo_ratio = m_nchOut_pseudo / m_nchOut;
			if (!((int) dstChPos % (int) pseudo_ratio)
					&& (int) srcChPos < (int) m_nchIn) {
				//	    fprintf(stderr,"dstChPos, pseudoRation %d : %d -- %d\n",dstChPos,pseudo_ratio,m_kernelWritePos);
				kernel[nKernels] =
						m_kernelArray[dstChPos / pseudo_ratio][srcChPos][yPos][xPos];
				//		writeKernelValue(m_kernelArray[dstChPos/pseudo_ratio][srcChPos][yPos][xPos]);
				//	    fprintf(stderr,"writing kernel value %d at %d:%d:%d:%d\n",m_kernelArray[dstChPos/pseudo_ratio][srcChPos][yPos][xPos],dstChPos/pseudo_ratio,srcChPos,yPos,xPos);
			}

			else {
				kernel[nKernels] = 0;
				//		writeKernelValue(0);
			}

			++m_kernelWritePos;
			if (m_kernelWritePos % m_contiguous_kernels == 0)
				break;
		}

		if (nKernels == 0)
			writeKernelValue(kernel, 1);
		else
			writeKernelValue(kernel, 3);

		m_completedKernelWrite = ((int) m_kernelWritePos
				== (int) (m_nchIn_pseudo * m_nchOut_pseudo * m_wk * m_hk));

	}

	else if ((int) m_kernelWritePos
			== (int) (m_nchIn_pseudo * m_nchOut_pseudo * m_wk * m_hk)) {
		//fprintf(stderr, "sending kernel write complete pulse\n");
		sendConfigData(config_kernel_memory_write_complete_pulse, 1);
		//fprintf(stderr, "finished sending kernel write complete pulse\n");
		++m_kernelWritePos;
	}

	else if (!m_sent_start_pulse) {
		sendConfigData(config_start_process_pulse, 1);
		m_sent_start_pulse = true;
	}

	else if (!m_sent_image_ready && m_currentInputPass != 0) {
		//fprintf(stderr, "sending image ready signal in pass %d\n",
		//		m_currentInputPass);
		sendConfigData(CONFIG_TYPE(20), 1);
		m_completedImageWrite = true;
		m_sent_image_ready = true;
	}

	else if (!m_completedImageWrite) {
		if (!m_imageCompressionEnabled) {
			int pixels[2];
			bool pixel2Valid = true;
			for (unsigned int iter = 0; iter < 2; ++iter) {
				unsigned int tempPos = m_imageWritePos;

				unsigned int srcChannel = tempPos % m_nchIn;
				tempPos /= m_nchIn;

				unsigned int xPos = tempPos % m_imageWidth;
				tempPos /= m_imageWidth;

				unsigned int yPos = tempPos;
				if (srcChannel == 0 && xPos == 0) {
					instruction[iter] = 15;
					old_row = yPos;
				}

				pixels[iter] = m_image[srcChannel][yPos][xPos];
				++m_imageWritePos;
				m_completedImageWrite = ((int) m_imageWritePos
						>= (int) (m_nchIn * m_hinMax * m_imageWidth));
				if (m_completedImageWrite && iter == 0) {
					pixel2Valid = false;
					break;
				}
			}

			fprintf(m_logPixels,
					"pos : %d . writing pixels: no compression %d %d\n ",
					m_imageWritePos, pixels[0], pixels[1]);
			writePixels(pixels[0], pixels[1], pixel2Valid, instruction);
			memset(instruction, 0, 2 * sizeof(int));

		}

		else {
			bool onePixelRemaining = false;
			int remainingPixel;
			bool done = ((int) m_imageWritePos
					>= (int) (m_nchIn * m_hinMax * m_imageWidth));
			if (m_pixelArrayWritePos + 1 < m_nPixelsArray) {
				writePixels(m_pixelArray[m_pixelArrayWritePos],
						m_pixelArray[m_pixelArrayWritePos + 1], true,
						instruction);
				memset(instruction, 0, 2 * sizeof(int));
				fprintf(m_logPixels,
						"pos : %d . writing pixels: no sparsity map %d %d\n ",
						m_imageWritePos, m_pixelArray[m_pixelArrayWritePos],
						m_pixelArray[m_pixelArrayWritePos + 1]);
				m_pixelArrayWritePos += 2;
				if (done && m_pixelArrayWritePos == m_nPixelsArray)
					m_completedImageWrite = true;
			}

			else if (m_pixelArrayWritePos == m_nPixelsArray - 1 && done) {
				writePixels(m_pixelArray[m_pixelArrayWritePos], 0, false,
						instruction);
				memset(instruction, 0, 2 * sizeof(int));

				fprintf(m_logPixels, "Writing final pixel %d\n ",
						m_pixelArray[m_pixelArrayWritePos]);
				m_completedImageWrite = true;
			}

			else {
				unsigned int SM = 0;
				if (m_pixelArrayWritePos < m_nPixelsArray) {
					onePixelRemaining = true;
					remainingPixel = m_pixelArray[m_pixelArrayWritePos];
				}

				for (int empty_iter = 0; empty_iter < 2; ++empty_iter) {
					m_nPixelsArray = m_pixelArrayWritePos = 0;
					for (int iter = 0; iter < 16; ++iter) {
						unsigned int tempPos = m_imageWritePos;

						unsigned int srcChannel = tempPos % m_nchIn;
						tempPos /= m_nchIn;

						unsigned int xPos = tempPos % m_imageWidth;
						tempPos /= m_imageWidth;

						unsigned int yPos = tempPos;
						done = ((int) m_imageWritePos
								>= (int) (m_nchIn * m_hinMax * m_imageWidth));

						if (srcChannel == 0 && xPos == 0) {
							instruction[empty_iter] = 15;
							old_row = yPos;
						}
						//			fprintf(m_logPixels,"At position %d(%d:%d:%d) in iterations %d:%d with pixel %d\n",m_imageWritePos,xPos,yPos,srcChannel,iter,empty_iter,done ? 0 : m_image[srcChannel][yPos][xPos]);

						if (done) {
							++m_imageWritePos;
							break;
						}

						if (srcChannel == 0 && xPos == 0 && yPos != 0
								&& iter != 0)
							break;

						//		    printf("before pixel acquisition %d %d %d\n",srcChannel,yPos,xPos);
						int pixel = m_image[srcChannel][yPos][xPos];
						//		    printf("after pixel acquisition\n");

						if (pixel != 0) {
							m_pixelArray[m_nPixelsArray++] = pixel;
							SM |= (1 << iter);
						}

						++m_imageWritePos;
					}

					if (onePixelRemaining) {
						instruction[1] = instruction[0]; //we are in empty_iter=1
						instruction[0] = 0;
						writePixels(remainingPixel, SM, true, instruction);
						memset(instruction, 0, 2 * sizeof(int));
						fprintf(m_logPixels,
								"writing remaining pixel and sparsity map %d %d\n ",
								remainingPixel, SM);

						break; //from empty_iter
					}

					else if (empty_iter == 1) {
						if (SM == 0 && done)
							m_completedImageWrite = true;
						else {
							writePixels(0, SM, true, instruction);
							memset(instruction, 0, 2 * sizeof(int));
							fprintf(m_logPixels,
									"pos:%d writing sparsity map after empty sparsity map %d\n ",
									m_imageWritePos, SM);
						}
					}

					else if (SM != 0) {
						writePixels(SM, m_pixelArray[m_pixelArrayWritePos],
						true, instruction);
						memset(instruction, 0, 2 * sizeof(int));

						fprintf(m_logPixels,
								"writing new sparsity map and first pixel %d %d\n ",
								SM, m_pixelArray[m_pixelArrayWritePos]);

						++m_pixelArrayWritePos;
						break; //from empty_iter
					}
				}
			}

		}
	} else if (!m_wroteImageDone) {
		sendConfigData(config_input_image_done, 1);
		m_wroteImageDone = true;
	} else
		axiWriteCommit();
}

void zs_driverMonitor::dumpImage() {
	char temp[100] = "res_";
	char buffer[30];
	sprintf(buffer, "%d", m_currentLayer);
	strcat(temp, buffer);

	FILE * fp;
	fp = fopen(temp, "w");
	if (fp == NULL) {
		printf("error opening file\n");
	}

	for (int j = 0; j < m_hinMax; ++j) {
		for (int k = 0; k < m_imageWidth; ++k) {
			for (int i = 0; i < m_nchIn; ++i)
				fprintf(fp, "%d ", m_image[i][j][k]);
			fprintf(fp, "\t");
		}
		fprintf(fp, "\n");
	}

	fclose(fp);
}

void zs_driverMonitor::dumpWaveforms(unsigned int currentStep) {
	for (unsigned int i = 0; i < 8; ++i) {
		char fileName[100];
		sprintf(fileName, "wf_compute%d", i);
		m_compute_wfs[i].dump(fileName);
	}
	m_writeConfig_wf.dump("wf_writeConfig");
	m_writeKernel_wf.dump("wf_writeKernel");
	m_writePixel_wf.dump("wf_writePixel");
	m_writeBias_wf.dump("wf_writeBias");
	m_readPixel_wf.dump("wf_readPixel");

	fprintf(m_layerInfo,
			"macsPerChannel %d endStep %d kernel_size %d imageWidth %d imageHeight %d nInputChannels %d nOutputChannels %d inputPadding %d pooling %d relu %d currentPass %d numPasses %d sparsity %f layerNumber %d\n",
			m_macs_per_channel, currentStep, m_wk, m_imageWidth, m_hinMax,
			m_nchIn, m_nchOut, m_inputLayerPadding, m_poolingEnabled,
			m_reluEnabled, m_currentInputPass, m_numInputPasses, getSparsity(),
			m_currentLayer);
	fflush(m_layerInfo);
}

double zs_driverMonitor::getSparsity() {
	//    fprintf(stderr,"getting sparsity for output of layer %d m_curren
	int outputWidth = m_layerParams[m_currentLayer + 1].num_input_column;
	int outputHeight = m_layerParams[m_currentLayer + 1].num_input_rows;
	int num_output_channels = m_layerParams[m_currentLayer].num_output_channels;
	unsigned int numZeros = 0;
	for (int i = 0; i < num_output_channels; ++i)
		for (int j = 0; j < outputHeight; ++j)
			for (int k = 0; k < outputWidth; ++k)
				if (m_outputImage[i][j][k] == 0)
					++numZeros;
	return (numZeros * 1.0 / (outputWidth * outputHeight * num_output_channels));
}

int zs_driverMonitor::processingLoop(unsigned int currentStep) {

	/*if (m_currentLayer >= m_numLayers) {
	 sleep(1);
	 }*/

	if (!m_activeProcessing) {

		if (setCurrentLayer(m_currentLayer) == FINISHED) {
			printf("got here\n");
			return (FINISHED);
		}
		printf("before initialize internal\n");
		initializeInternalVariables();
		initializeConfigArray();
	}

	phase2_step();

	m_activeProcessing = true;

	if (m_completedImageWrite && m_gotAllPixels) {
		dumpWaveforms(currentStep);
		m_activeProcessing = false;
		++m_currentInputPass;
		fprintf(stderr,
				"+++++++++ Finished processing layer %d, pass %d of %d\n",
				m_currentLayer, m_currentInputPass, m_numInputPasses);
		if (m_currentInputPass == m_numInputPasses) {
			m_currentInputPass = 0;
			++m_currentLayer;
		}

		if (m_currentLayer == m_numLayers) {

			fprintf(stderr,
					"\n\n\n ************** evaluating FC layers **************\n");
			evaluateFCLayers();

			/*fprintf(stderr, "\n *** \n First FC layer output: \n");
			 for (unsigned int i = 0; i < IP1_OP_SIZE; i++) {
			 fprintf(stderr, "m_fc1_output[%d]: %d \n", i, m_fc1_output[i]);
			 }*/

			fprintf(stderr, "\n *** \n Second FC layer output: \n");
			for (unsigned int i = 0; i < IP2_OP_SIZE; i++) {
				fprintf(stderr, "m_fc2_output[%d]: %d \n", i, m_fc2_output[i]);
			}

			if (error_counter > 0) {
				fprintf(stderr,
						"\n\n\n\n +++++++++ SIMULATION IS OVER +++++++++ \n\n");
				fprintf(stderr, " +++   total error count is: %d ",
						error_counter);
				fprintf(stderr,
						"\n\n ++++++++++++++++++++++++++++++++++++++ \n\n\n\n");
			}

			else {
				fprintf(stderr,
						"\n\n\n\n --------- SIMULATION IS OVER --------- \n\n");
				fprintf(stderr,
						"\n\n\n\n --------- NO ERRORS ARE COUNTED ------ \n\n");
			}
		}

	}

	return (1);
}

void zs_driverMonitor::dumpKernels() {
	char temp[100] = "kernels_";
	char buffer[30];
	sprintf(buffer, "%d", m_currentLayer);
	strcat(temp, buffer);

	FILE * fp;

	fp = fopen(temp, "w");
	unsigned int index = 0;
	for (int i = 0; i < m_nchOut; ++i) {
		index = 0;
		for (int j = 0; j < m_nchIn; ++j) {
			for (int k = 0; k < m_hk; ++k) {
				for (int l = 0; l < m_wk; ++l) {
					fprintf(fp, "%d ", m_kernelArray[i][j][k][l]);
					++index;
					if (!(index % (m_wk * m_hk)))
						fprintf(fp, "\n");
				}
			}
		}
		fprintf(fp, "\n\n\n");
	}

	// fprintf(fp,"biases\n\n\n");
	// for(unsigned int i=0;i<NUM_MAC_BLOCKS;++i)
	//   {
	// 	fprintf(fp,"%d",m_biases[i]);
	// 	if(!(i%m_macs_per_channel))
	// 	  fprintf(fp,"  <-----active bias");
	// 	fprintf(fp,"\n");
	//   }

	fclose(fp);
}

void zs_driverMonitor::dumpPixels(const char * fileName) {
	FILE * fp;

	fp = fopen(fileName, "w");
	unsigned int index = 0;
	for (int k = 0; k < m_hinMax; ++k) {
		for (int l = 0; l < m_imageWidth; ++l) {
			for (int i = 0; i < m_nchIn; ++i) {
				fprintf(fp, "%d ", m_image[i][k][l]);
				++index;
			}
			fprintf(fp, "\t");
		}
		fprintf(fp, "\n\n");
	}

	fclose(fp);

}

int zs_driverMonitor::runLoop() {

	unsigned long long n_clkCycles = 0;

	while (1) {
		++n_clkCycles;
		if (processingLoop(n_clkCycles) == FINISHED) {
			break;
		}
	}
	printf("got also here\n");
	return (1);

}

void * readThreadRoutine(void * arg) {

	zs_driverMonitor *zsDM = ((zs_driverMonitor *) arg);
	zsDM->m_readAxiFile = fopen("/home/root/readAxiFile", "w");
	if (zsDM->m_readAxiFile == NULL) {
		printf("error opening readaxiFile\n");
		exit(1);
	}
	while (1) {
		zsDM->readFromAxi();
	}

}