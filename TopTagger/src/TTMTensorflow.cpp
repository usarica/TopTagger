#include "TopTagger/TopTagger/interface/TTMTensorflow.h"

#include "TopTagger/TopTagger/interface/TopTaggerUtilities.h"
#include "TopTagger/TopTagger/interface/TopObject.h"
#include "TopTagger/TopTagger/interface/TopTaggerResults.h"
#include "TopTagger/CfgParser/include/Context.hh"
#include "TopTagger/CfgParser/include/CfgDocument.hh"
#include "TopTagger/CfgParser/include/TTException.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

void TTMTensorflow::getParameters(const cfg::CfgDocument* cfgDoc, const std::string& localContextName)
{
#ifdef DOTENSORFLOW
    //Construct contexts
    cfg::Context commonCxt("Common");
    cfg::Context localCxt(localContextName);

    discriminator_ = cfgDoc->get("discCut",       localCxt, -999.9);
    discOffset_    = cfgDoc->get("discOffset",    localCxt, 999.9);
    discSlope_     = cfgDoc->get("discSlope",     localCxt, 0.0);
    modelFile_     = cfgDoc->get("modelFile",     localCxt, "");
    inputOp_       = cfgDoc->get("inputOp",       localCxt, "x");
    outputOp_      = cfgDoc->get("outputOp",      localCxt, "y");
    NConstituents_ = cfgDoc->get("NConstituents", localCxt, 3);

    csvThreshold_  = cfgDoc->get("csvThreshold", localCxt, -999.9);
    bEtaCut_       = cfgDoc->get("bEtaCut",      localCxt, -999.9);
    maxNbInTop_    = cfgDoc->get("maxNbInTop",   localCxt, -1);

    std::string modelFileFullPath;
    if(workingDirectory_.size()) modelFileFullPath = workingDirectory_ + "/" + modelFile_;
    else                         modelFileFullPath = modelFile_;

    int iVar = 0;
    bool keepLooping;
    do
    {
        keepLooping = false;

        //Get variable name
        std::string varName = cfgDoc->get("mvaVar", iVar, localCxt, "");

        //if it is a non empty string save in vector
        if(varName.size() > 0)
        {
            keepLooping = true;

            vars_.push_back(varName);
        }
        ++iVar;
    }
    while(keepLooping);

    //Variable to hold tensorflow status
    TF_Status* status = TF_NewStatus();

    //get the grafdef from the file
    TF_Buffer* graph_def = read_file(modelFileFullPath);

    // Import graph_def into graph
    TF_Graph* graph = TF_NewGraph();
    TF_ImportGraphDefOptions* graph_opts = TF_NewImportGraphDefOptions();
    TF_GraphImportGraphDef(graph, graph_def, graph_opts, status);
    TF_DeleteImportGraphDefOptions(graph_opts);
    TF_DeleteBuffer(graph_def);

    if (TF_GetCode(status) != TF_OK) 
    {
        THROW_TTEXCEPTION("ERROR: Unable to import graph: " + std::string(TF_Message(status)));
    }

    //Create tensorflow session from imported graph
    TF_SessionOptions* sess_opts = TF_NewSessionOptions();
    //serialized configuration protobuffer indicating to set session intra_op_parallelism setting to 1
    uint8_t config[] = {0x10, 0x01};
    TF_SetConfig(sess_opts, static_cast<void*>(config), 2, status);
    session_ = TF_NewSession(graph, sess_opts, status);
    TF_DeleteSessionOptions(sess_opts);

    if (TF_GetCode(status) != TF_OK) 
    {
        THROW_TTEXCEPTION("ERROR: Unable to create tf session: " + std::string(TF_Message(status)));
    }

    TF_Operation* op_x = TF_GraphOperationByName(graph, inputOp_.c_str());
    TF_Operation* op_y = TF_GraphOperationByName(graph, outputOp_.c_str());

    //Clean up graph
    TF_DeleteGraph(graph);

    if(op_x == nullptr)
    {
        THROW_TTEXCEPTION("Input operation \"" + inputOp_ + "\" not found in graph");
    }    

    if(op_y == nullptr)
    {
        THROW_TTEXCEPTION("Output operation \"" + outputOp_ + "\" not found in graph");
    }

    inputs_ .emplace_back(TF_Output({op_x, 0}));
    outputs_.emplace_back(TF_Output({op_y, 0}));
    targets_.emplace_back(op_y);

    TF_DeleteStatus(status);

    //load variables
    if(NConstituents_ == 1)
    {
        varCalculator_.reset(new ttUtility::BDTMonojetInputCalculator());
    }
    else if(NConstituents_ == 2)
    {
        varCalculator_.reset(new ttUtility::BDTDijetInputCalculator());
    }
    else if(NConstituents_ == 3)
    {
        varCalculator_.reset(new ttUtility::TrijetInputCalculator());
    }
    //map variables
    varCalculator_->mapVars(vars_);

#else
    //Mark variables unused to suppress warnings
    (void)cfgDoc;
    (void)localContextName;
    THROW_TTEXCEPTION("ERROR: TopTagger not compiled with Tensorflow c-api support!!!");
#endif
}

void TTMTensorflow::run(TopTaggerResults& ttResults)
{
#ifdef DOTENSORFLOW
    //Get the list of top candidates as generated by the clustering algo
    std::vector<TopObject>& topCandidates = ttResults.getTopCandidates();
    //Get the list of final tops into which we will stick candidates
    std::vector<TopObject*>& tops = ttResults.getTops();


    std::vector<TopObject*> validCands;
    for(auto& topCand : topCandidates)
    {
        //Prepare data from top candidate (this code is shared with training tuple producer)
        if(varCalculator_->checkCand(topCand))
        {
            validCands.emplace_back(&topCand);
        }
    }

    //tensorflow status variable
    TF_Status* status = TF_NewStatus();

    //Create place to store the output vectors 
    std::vector<TF_Tensor*>    output_values(1);

    //Construct tensorflow input tensor
    std::vector<TF_Tensor*> input_values;
    const int elemSize = sizeof(float);
    std::vector<int64_t> dims = {static_cast<int64_t>(validCands.size()), static_cast<int64_t>(vars_.size())};
    int nelem = 1;
    for(const auto dimLen : dims) nelem *= dimLen;
    TF_Tensor* input_values_0 =  TF_AllocateTensor(TF_FLOAT, dims.data(), dims.size(), elemSize*nelem);

    input_values = { input_values_0 };
    varCalculator_->setPtr(static_cast<float*>(TF_TensorData(input_values_0)));

    //Prepare data from top candidate (this code is shared with training tuple producer)
    unsigned int iCand = 0;
    for(auto& topCand : validCands)
    {
        if(varCalculator_->calculateVars(*topCand, iCand)) ++iCand;
    }

    //predict values
    TF_SessionRun(session_,
                  // RunOptions
                  nullptr,
                  // Input tensors
                  inputs_.data(), input_values.data(), inputs_.size(),
                  // Output tensors
                  outputs_.data(), output_values.data(), outputs_.size(),
                  // Target operations
                  targets_.data(), targets_.size(),
                  // RunMetadata
                  nullptr,
                  // Output status
                  status);

    
    if (TF_GetCode(status) != TF_OK)
    {
        THROW_TTEXCEPTION("ERROR: Unable to run graph: " + std::string(TF_Message(status)));
    }

    //Get output discriminators 
    auto discriminators = static_cast<float*>(TF_TensorData(output_values[0]));                
    for(iCand = 0; iCand < validCands.size(); ++iCand)
    {
        auto* topCand = validCands[iCand];
        
        //discriminators is a 2D array, we only want the first entry of every array
        double discriminator = static_cast<double>(discriminators[iCand*TF_Dim(output_values[0], 1)]);
        topCand->setDiscriminator(discriminator);
        
        //Check number of b-tagged jets in the top
        bool passBrequirements = maxNbInTop_ < 0 || topCand->getNBConstituents(csvThreshold_, bEtaCut_) <= maxNbInTop_;
        
        //place in final top list if it passes the threshold
        if(discriminator > std::min(discriminator_, discOffset_ + topCand->p().Pt()*discSlope_) && passBrequirements)
        {
            tops.push_back(topCand);
        }
    }

    for(auto tensor : input_values)  TF_DeleteTensor(tensor);
    for(auto tensor : output_values) TF_DeleteTensor(tensor);

    TF_DeleteStatus(status);
#else
    //Mark variables unused to suppress warnings
    (void)ttResults;
#endif
}

TTMTensorflow::~TTMTensorflow()
{
#ifdef DOTENSORFLOW
    //tensorflow status variable
    TF_Status* status = TF_NewStatus();

    TF_DeleteSession(session_, status);

    if (TF_GetCode(status) != TF_OK)
    {
        //THROW_TTEXCEPTION("ERROR: Unable to delete tf session: " + std::string(TF_Message(status)));
    }

    TF_DeleteStatus(status);
#endif
}

#ifdef DOTENSORFLOW

void free_buffer(void* data, size_t length) 
{
    //mark length as unused
    (void)length;
    //free the memory
    free(data);
}

TF_Buffer* TTMTensorflow::read_file(const std::string& file) 
{
    FILE *f = fopen(file.c_str(), "rb");

    if(f == nullptr)
    {
        THROW_TTEXCEPTION("File not found: \"" + file + "\"");
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);  //same as rewind(f);

    void* data = malloc(fsize);
    fread(data, fsize, 1, f);
    fclose(f);

    TF_Buffer* buf = TF_NewBuffer();
    buf->data = data;
    buf->length = fsize;
    buf->data_deallocator = free_buffer;
    return buf;
}
#endif
