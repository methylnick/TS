/* Copyright (C) 2011 Ion Torrent Systems, Inc. All Rights Reserved */

#include <string>
#include <cassert>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "RegionAnalysis.h"
#include "DPTreephaser.h"

#define	MAX_CAFIE_READS_PER_REGION		5000
#define MAX_CAFIE_READS_PER_REGION2    2000

RegionAnalysis::RegionAnalysis()
{
  clo = NULL;
  mask = NULL;
  libraryInfo = NULL;
  cf = NULL;
  ie = NULL;
  dr = NULL;
  common_output_mutex = NULL;
  numFlows = 0;
}

void RegionAnalysis::analyze(vector<float> *_cf, vector<float> *_ie, vector<float> *_dr, RawWells *_wells, Mask *_mask, SequenceItem *_libraryInfo, CommandLineOpts *_clo,
    const string& _flowOrder, int _numFlows, int numWorkers)
{
  printf("RegionAnalysis::analyze start (with %d threads)\n", numWorkers);

  // By storing these values as class members, they become visible to all worker threads.
  cf = _cf;
  ie = _ie;
  dr = _dr;
  mask = _mask;
  libraryInfo = _libraryInfo;
  clo = _clo;

  flowOrder = _flowOrder;
  numFlows = _numFlows;

  // Initialize region wells reader

  assert (wellsReader.OpenForRead(_wells, mask->W(), mask->H(), clo->cfe_control.cfiedrRegionsX, clo->cfe_control.cfiedrRegionsY));

  // Launch worker threads and await their completion

  pthread_mutex_t tmp_mutex = PTHREAD_MUTEX_INITIALIZER;
  common_output_mutex = &tmp_mutex;

  pthread_t worker_id[numWorkers];

  for (int iWorker = 0; iWorker < numWorkers; iWorker++)
    if (pthread_create(&worker_id[iWorker], NULL, RegionAnalysisWorker, this))
      ION_ABORT("*Error* - problem starting thread");

  for (int iWorker = 0; iWorker < numWorkers; iWorker++)
    pthread_join(worker_id[iWorker], NULL);

  printf("RegionAnalysis::analyze end\n");
}



void *RegionAnalysisWorker(void *arg)
{
  RegionAnalysis *analysis = static_cast<RegionAnalysis*> (arg);

  if (analysis->clo->cfe_control.libPhaseEstimator == "nel-mead-treephaser")
    analysis->worker_Treephaser();
  else if (analysis->clo->cfe_control.libPhaseEstimator == "nel-mead-adaptive-treephaser")
    analysis->worker_AdaptiveTreephaser();


  return NULL;
}



void RegionAnalysis::worker_Treephaser()
{

  // Worker method: load regions one by one and process them until done

//  int numFlows = wells->NumFlows();

//  DPTreephaser dpTreephaser(wells->FlowOrder(), numFlows, 8);
  DPTreephaser dpTreephaser(flowOrder.c_str(), numFlows, 8);

  std::deque<int> wellX;
  std::deque<int> wellY;
  std::deque<std::vector<float> > wellMeasurements;
  int iRegion;

  std::vector<BasecallerRead> data;
  data.reserve(MAX_CAFIE_READS_PER_REGION);

  while (wellsReader.loadNextRegion(wellX, wellY, wellMeasurements, iRegion)) {

    float parameters[3];
    parameters[0] = 0.00; // CF - initial guess
    parameters[1] = 0.00; // IE - initial guess
    parameters[2] = 0.000; // DR - initial guess

    for (int globalIteration = 0; globalIteration < 5; globalIteration++) {

      dpTreephaser.SetModelParameters(parameters[0], parameters[1], parameters[2]);

      data.clear();

      // Iterate over live library wells and consider them as a part of the phase training set

      std::deque<int>::iterator x = wellX.begin();
      std::deque<int>::iterator y = wellY.begin();
      std::deque<std::vector<float> >::iterator measurements = wellMeasurements.begin();

      for (; (x != wellX.end()) && (data.size() < MAX_CAFIE_READS_PER_REGION); x++, y++, measurements++) {

        if (!mask->Match(*x, *y, MaskLive))
          continue;
        if (!mask->Match(*x, *y, MaskBead))
          continue;

        int beadClass = 1; // 1 - library, 0 - TF

        if (!mask->Match(*x, *y, MaskLib)) {  // Is it a library bead?
          if (!mask->Match(*x, *y, MaskTF))   // OK, is it at least a TF?
            continue;
          beadClass = 0;
        }

        data.push_back(BasecallerRead());

        data.back().SetDataAndKeyNormalize(&(measurements->at(0)), numFlows, libraryInfo[beadClass].Ionogram, libraryInfo[beadClass].numKeyFlows - 1);

        bool keypass = true;
        for (int iFlow = 0; iFlow < (libraryInfo[beadClass].numKeyFlows - 1); iFlow++) {
          if ((int) (data.back().measurements[iFlow] + 0.5) != libraryInfo[beadClass].Ionogram[iFlow])
            keypass = false;
          if (isnan(data.back().measurements[iFlow]))
            keypass = false;
        }

        if (!keypass) {
          data.pop_back();
          continue;
        }

        dpTreephaser.Solve(data.back(), std::min(100, numFlows));
        data.back().Normalize(11, std::min(80, numFlows));
        dpTreephaser.Solve(data.back(), std::min(120, numFlows));
        data.back().Normalize(11, std::min(100, numFlows));
        dpTreephaser.Solve(data.back(), std::min(120, numFlows));


        float metric = 0;
        for (int iFlow = 20; (iFlow < 100) && (iFlow < numFlows); iFlow++) {
          if (data.back().normalizedMeasurements[iFlow] > 1.2)
            continue;
          float delta = data.back().normalizedMeasurements[iFlow] - data.back().prediction[iFlow];
          if (!isnan(delta))
            metric += delta * delta;
          else
            metric += 1e10;
        }

        if (metric > 1) {
          data.pop_back();
          continue;
        }

      }

      if (data.size() < 10)
        break;

      // Perform parameter estimation

      NelderMeadOptimization(data, dpTreephaser, parameters, 50, 3);
    }

    pthread_mutex_lock(common_output_mutex);
    if (data.size() < 10)
      printf("Region % 3d: Using default phase parameters, %d reads insufficient for training\n", iRegion + 1, (int) data.size());
    else
      printf("Region % 3d: Using %d reads for phase parameter training\n", iRegion + 1, (int) data.size());
    //      printf("o");
    fflush(stdout);
    pthread_mutex_unlock(common_output_mutex);

    (*cf)[iRegion] = parameters[0];
    (*ie)[iRegion] = parameters[1];
    (*dr)[iRegion] = parameters[2];

  }

}





void RegionAnalysis::worker_AdaptiveTreephaser()
{

  // Worker method: load regions one by one and process them until done

//  int numFlows = wells->NumFlows();

//  DPTreephaser dpTreephaser(wells->FlowOrder(), numFlows, 8);
  DPTreephaser dpTreephaser(flowOrder.c_str(), numFlows, 8);

  std::deque<int> wellX;
  std::deque<int> wellY;
  std::deque<std::vector<float> > wellMeasurements;
  int iRegion;

  std::vector<BasecallerRead> data;
  data.reserve(MAX_CAFIE_READS_PER_REGION2);

  while (wellsReader.loadNextRegion(wellX, wellY, wellMeasurements, iRegion)) {

    float parameters[3];
    parameters[0] = 0.00; // CF - initial guess
    parameters[1] = 0.00; // IE - initial guess
    parameters[2] = 0.000; // DR - initial guess


    for (int globalIteration = 0; globalIteration < 5; globalIteration++) {

      dpTreephaser.SetModelParameters(parameters[0], parameters[1], parameters[2]);

      data.clear();

      // Iterate over live library wells and consider them as a part of the phase training set

      std::deque<int>::iterator x = wellX.begin();
      std::deque<int>::iterator y = wellY.begin();
      std::deque<std::vector<float> >::iterator measurements = wellMeasurements.begin();

      for (; (x != wellX.end()) && (data.size() < MAX_CAFIE_READS_PER_REGION2); x++, y++, measurements++) {

        if (!mask->Match(*x, *y, MaskLive))
          continue;
        if (!mask->Match(*x, *y, MaskBead))
          continue;

        int beadClass = 1; // 1 - library, 0 - TF

        if (!mask->Match(*x, *y, MaskLib)) {  // Is it a library bead?
          if (!mask->Match(*x, *y, MaskTF))   // OK, is it at least a TF?
            continue;
          beadClass = 0;
        }

        data.push_back(BasecallerRead());

        data.back().SetDataAndKeyNormalize(&(measurements->at(0)), numFlows, libraryInfo[beadClass].Ionogram, libraryInfo[beadClass].numKeyFlows - 1);

        bool keypass = true;
        for (int iFlow = 0; iFlow < (libraryInfo[beadClass].numKeyFlows - 1); iFlow++) {
          if ((int) (data.back().measurements[iFlow] + 0.5) != libraryInfo[beadClass].Ionogram[iFlow])
            keypass = false;
          if (isnan(data.back().measurements[iFlow]))
            keypass = false;
        }

        if (!keypass) {
          data.pop_back();
          continue;
        }

//        dpTreephaser.NormalizeAndSolve3(data.back(),std::min(150, numFlows));
        dpTreephaser.NormalizeAndSolve3(data.back(),std::min(200, numFlows));

        float ppf_numerator = 0;
        float ppf_denominator = 0;
        for (int iFlow = 11; (iFlow < 100) && (iFlow < numFlows); iFlow++) {
          ppf_denominator += 1.0;
          if (data.back().solution[iFlow] > 0)
            ppf_numerator += 1.0;
        }
        ppf_numerator /= ppf_denominator;

        if (ppf_numerator > 0.6) {
          data.pop_back();
          continue;
        }


        float metric = 0;
        for (int iFlow = 20; (iFlow < 100) && (iFlow < numFlows); iFlow++) {
          if (data.back().normalizedMeasurements[iFlow] > 1.2)
            continue;
          float delta = data.back().normalizedMeasurements[iFlow] - data.back().prediction[iFlow];
          metric += delta * delta;
        }

        if ((metric > 2) || isnan(metric)) {
          data.pop_back();
          continue;
        }

      }

      if (data.size() < 10)
        break;

      // Perform parameter estimation

      NelderMeadOptimization(data, dpTreephaser, parameters, 50, 2);
    }

    pthread_mutex_lock(common_output_mutex);
    if (data.size() < 10)
      printf("Region % 3d: Using default phase parameters, %d reads insufficient for training\n", iRegion + 1, (int) data.size());
    else
      printf("Region % 3d: Using %d reads for phase parameter training\n", iRegion + 1, (int) data.size());
    //      printf("o");
    fflush(stdout);
    pthread_mutex_unlock(common_output_mutex);

    (*cf)[iRegion] = parameters[0];
    (*ie)[iRegion] = parameters[1];
    (*dr)[iRegion] = parameters[2];

  }

}





float RegionAnalysis::evaluateParameters(std::vector<BasecallerRead> &dataAll, DPTreephaser& treephaser, float *parameters)
{
  float metric = 0;
  if (clo->cfe_control.libPhaseEstimator == "nel-mead-treephaser") {

    if (parameters[0] < 0) // cf
      metric = 1e10;
    if (parameters[1] < 0) // ie
      metric = 1e10;
    if (parameters[2] < 0) // dr
      metric = 1e10;

    if (parameters[0] > 0.04) // cf
      metric = 1e10;
    if (parameters[1] > 0.04) // ie
      metric = 1e10;
    if (parameters[2] > 0.01) // dr
      metric = 1e10;

    if (metric == 0) {

      treephaser.SetModelParameters(parameters[0], parameters[1], parameters[2]);
      for (std::vector<BasecallerRead>::iterator data = dataAll.begin(); data != dataAll.end(); data++) {

        treephaser.Simulate3(*data, 120);
        data->Normalize(20, 100);

        for (int iFlow = 20; iFlow < std::min(100, data->numFlows); iFlow++) {
          if (data->measurements[iFlow] > 1.2)
            continue;
          float delta = data->measurements[iFlow] - data->prediction[iFlow] * data->miscNormalizer;
          metric += delta * delta;
        }
      }

    }

  } else if (clo->cfe_control.libPhaseEstimator == "nel-mead-adaptive-treephaser") {

    if (parameters[0] < 0) // cf
      metric = 1e10;
    if (parameters[1] < 0) // ie
      metric = 1e10;

    if (parameters[0] > 0.04) // cf
      metric = 1e10;
    if (parameters[1] > 0.04) // ie
      metric = 1e10;

    if (metric == 0) {

      treephaser.SetModelParameters(parameters[0], parameters[1], 0);
      for (std::vector<BasecallerRead>::iterator data = dataAll.begin(); data != dataAll.end(); data++) {

//        treephaser.Simulate3(*data, 150);
//        data->AdaptiveNormalizationOfPredictions(3, 50);
        treephaser.Simulate3(*data, 200);
        data->AdaptiveNormalizationOfPredictions(4, 50);

        for (int iFlow = 25; iFlow < std::min(175, data->numFlows); iFlow++) {
          if (data->measurements[iFlow] > 1.2)
            continue;
          float delta = data->measurements[iFlow] - data->prediction[iFlow];
          metric += delta * delta;
        }
      }
    }
  }
  //  printf("CF = %1.2f%%  IE = %1.2f%%  DR = %1.2f%%  V = %1.6f\n",
  //      100*parameters[0], 100*parameters[1], 100*parameters[2], metric);
  if (isnan(metric))
    metric = 1e10;

  return metric;
}





#define NMalpha   1.0
#define NMgamma   2.0
#define NMrho   0.5
#define NMsigma   0.5


void RegionAnalysis::NelderMeadOptimization (std::vector<BasecallerRead> &dataAll, DPTreephaser& treephaser,
    float *parameters, int numEvaluations, int numParameters)
{

  int iEvaluation = 0;

  //
  // Step 1. Pick initial vertices, evaluate the function at vertices, and sort the vertices
  //

  float   vertex[numParameters+1][numParameters];
  float   value[numParameters+1];
  int     order[numParameters+1];

  for (int iVertex = 0; iVertex <= numParameters; iVertex++) {

    for (int iParam = 0; iParam < numParameters; iParam++)
      vertex[iVertex][iParam] = parameters[iParam];

        switch (iVertex) {
        case 0:                 // First vertex just matches the provided starting values
            break;
        case 1:                 // Second vertex has higher CF
            vertex[iVertex][0] += 0.004;
            break;
        case 2:                 // Third vertex has higher IE
            vertex[iVertex][1] += 0.004;
            break;
        case 3:                 // Fourth vertex has higher DR
            vertex[iVertex][2] += 0.001;
            break;
        default:                // Default for future parameters
            vertex[iVertex][iVertex-1] *= 1.5;
            break;
        }

    value[iVertex] = evaluateParameters(dataAll, treephaser, vertex[iVertex]);
    iEvaluation++;

    order[iVertex] = iVertex;

    for (int xVertex = iVertex; xVertex > 0; xVertex--) {
      if (value[order[xVertex]] < value[order[xVertex-1]]) {
        int x = order[xVertex];
        order[xVertex] = order[xVertex-1];
        order[xVertex-1] = x;
      }
    }
  }

  // Main optimization loop

  while (iEvaluation < numEvaluations) {

    //
    // Step 2. Attempt reflection (and possibly expansion)
    //

    float center[numParameters];
    float reflection[numParameters];

    int worst = order[numParameters];
    int secondWorst = order[numParameters-1];
    int best = order[0];

    for (int iParam = 0; iParam < numParameters; iParam++) {
      center[iParam] = 0;
      for (int iVertex = 0; iVertex <= numParameters; iVertex++)
        if (iVertex != worst)
          center[iParam] += vertex[iVertex][iParam];
      center[iParam] /= numParameters ;
      reflection[iParam] = center[iParam] + NMalpha * (center[iParam] - vertex[worst][iParam]);
    }

    float reflectionValue = evaluateParameters(dataAll, treephaser, reflection);
    iEvaluation++;

    if (reflectionValue < value[best]) {    // Consider expansion:

      float expansion[numParameters];
      for (int iParam = 0; iParam < numParameters; iParam++)
        expansion[iParam] = center[iParam] + NMgamma * (center[iParam] - vertex[worst][iParam]);
      float expansionValue = evaluateParameters(dataAll, treephaser, expansion);
      iEvaluation++;

      if (expansionValue < reflectionValue) {   // Expansion indeed better than reflection
        for (int iParam = 0; iParam < numParameters; iParam++)
          reflection[iParam] = expansion[iParam];
        reflectionValue = expansionValue;
      }
    }

    if (reflectionValue < value[secondWorst]) { // Either reflection or expansion was successful

      for (int iParam = 0; iParam < numParameters; iParam++)
        vertex[worst][iParam] = reflection[iParam];
      value[worst] = reflectionValue;

      for (int xVertex = numParameters; xVertex > 0; xVertex--) {
        if (value[order[xVertex]] < value[order[xVertex-1]]) {
          int x = order[xVertex];
          order[xVertex] = order[xVertex-1];
          order[xVertex-1] = x;
        }
      }
      continue;
    }


    //
    // Step 3. Attempt contraction (reflection was unsuccessful)
    //

    float contraction[numParameters];
    for (int iParam = 0; iParam < numParameters; iParam++)
      contraction[iParam] = vertex[worst][iParam] + NMrho * (center[iParam] - vertex[worst][iParam]);
    float contractionValue = evaluateParameters(dataAll, treephaser, contraction);
    iEvaluation++;

    if (contractionValue < value[worst]) {  // Contraction was successful

      for (int iParam = 0; iParam < numParameters; iParam++)
        vertex[worst][iParam] = contraction[iParam];
      value[worst] = contractionValue;

      for (int xVertex = numParameters; xVertex > 0; xVertex--) {
        if (value[order[xVertex]] < value[order[xVertex-1]]) {
          int x = order[xVertex];
          order[xVertex] = order[xVertex-1];
          order[xVertex-1] = x;
        }
      }
      continue;
    }


    //
    // Step 4. Perform reduction (contraction was unsuccessful)
    //

    for (int iVertex = 1; iVertex <= numParameters; iVertex++) {

      for (int iParam = 0; iParam < numParameters; iParam++)
        vertex[order[iVertex]][iParam] = vertex[best][iParam] + NMsigma * (vertex[order[iVertex]][iParam] - vertex[best][iParam]);

      value[order[iVertex]] = evaluateParameters(dataAll, treephaser, vertex[order[iVertex]]);
      iEvaluation++;

      for (int xVertex = iVertex; xVertex > 0; xVertex--) {
        if (value[order[xVertex]] < value[order[xVertex-1]]) {
          int x = order[xVertex];
          order[xVertex] = order[xVertex-1];
          order[xVertex-1] = x;
        }
      }
    }
  }

  for (int iParam = 0; iParam < numParameters; iParam++)
    parameters[iParam] = vertex[order[0]][iParam];
}

