// stdafx.h : file di inclusione per file di inclusione di sistema standard
// o file di inclusione specifici del progetto utilizzati di frequente, ma
// modificati raramente
//

#pragma once

// ~-----custom headers----------------------------------------
#include "targetver.h"
#include "Coordinates.h"
/*#include "Detection.h"
#include "Packet.h"
#include "Position.h"
#include "BlockingQueue_Aggregator.h"
#include "BlockingQueue_Interpolator.h"
#include "Aggregator.h"
#include "Interpolator.h"
#include "Synchronizer.h"*/


// ~-----C libraries-------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <time.h>
#include <string.h>
#include <limits.h>
#include <time.h>


// ~-----C++ libraries-----------------------------------------
#include <string>
#include <iostream>
#include <vector>
#include <chrono>
#include <exception>
#include <map>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sstream>
#include <list>
#include <vector>
#include <deque>
#include <set>
#include <algorithm>
#include <memory>
#include <iterator>
#include <random>
#include <thread>
#include <fstream>
#include <locale>
#include <limits>


// ~-----constants---------------------------------------------
#define DEBUG true
#define FILE_CHART true
#define FILE_PERFORMANCE true


#if DEBUG
	static std::mutex _m_console;

#endif
