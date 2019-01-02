#ifdef DOPYCAPIBIND
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include "Python.h"
#include "numpy/arrayobject.h"

#include "TLorentzVector.h"

#include <vector>
#include <iostream>

#include "TopTagger/TopTagger/interface/TopTagger.h"
#include "TopTagger/TopTagger/interface/TopTaggerResults.h"
#include "TopTagger/TopTagger/interface/TopTaggerPython.h"
#include "TopTagger/TopTagger/interface/TopTaggerUtilities.h"
#include "TopTagger/CfgParser/include/TTException.h"

extern "C"
{

    /// Destructor function for TopTagger object cleanup 
    static void TopTaggerInterface_cleanup(PyObject *ptt)
    {
        //Get top tagger pointer from capsule 
        TopTagger* tt = (TopTagger*) PyCapsule_GetPointer(ptt, "TopTagger");

        if(tt) delete tt;
    }

    static PyObject* TopTaggerInterface_setup(PyObject *self, PyObject *args)
    {
        //suppress unused parameter warning as self is manditory
        (void)self;

        char *cfgFile, *workingDir = nullptr;

        if (!PyArg_ParseTuple(args, "s|s", &cfgFile, &workingDir)) {
            PyErr_SetString(PyExc_TypeError, "Incorrect function parameters");
            return NULL;
        }

        //Setup top tagger 
        TopTagger *tt = nullptr;

        try
        {
            tt = new TopTagger();

            //Check that "new" succeeded
            if(!tt)
            {
                PyErr_NoMemory();
                return NULL;
            }

            //Disable internal print statements on exception 
            tt->setVerbosity(0);

            if(workingDir && strlen(workingDir) > 0)
            {
                tt->setWorkingDirectory(workingDir);
            }
            tt->setCfgFile(cfgFile);
        }
        catch(const TTException& e)
        {
            std::cout << "TopTagger exception message: " << e << std::endl;
            PyErr_SetString(PyExc_RuntimeError, "TopTagger exception thrown (look above to find specific exception message)");
            return NULL;
        }

        PyObject * ret = PyCapsule_New(tt, "TopTagger", TopTaggerInterface_cleanup);

        return Py_BuildValue("N", ret);
    }

    static PyObject* TopTaggerInterface_run(PyObject *self, PyObject *args)
    {
        //suppress unused parameter warning as self is manditory
        (void)self;

        PyObject *ptt, *pJetPt, *pJetEta, *pJetPhi, *pJetMass, *pJetBtag, *pFloatVarsDict, *pIntVarsDict;
        if (!PyArg_ParseTuple(args, "O!OOOOOO!O!", &PyCapsule_Type, &ptt, &pJetPt, &pJetEta, &pJetPhi, &pJetMass, &pJetBtag, &PyDict_Type, &pFloatVarsDict, &PyDict_Type, &pIntVarsDict))
        {
            PyErr_SetString(PyExc_TypeError, "Incorrect function parameters");
            return NULL;
        }

        Py_INCREF(ptt);
        Py_INCREF(pJetPt);
        Py_INCREF(pJetEta);
        Py_INCREF(pJetPhi);
        Py_INCREF(pJetMass);
        Py_INCREF(pJetBtag);
        Py_INCREF(pFloatVarsDict);
        Py_INCREF(pIntVarsDict);

        //Prepare std::vector<TLorentzVector> for jets lorentz vectors
        ttPython::Py_buffer_wrapper<Float_t> jetPt(pJetPt);
        ttPython::Py_buffer_wrapper<Float_t> jetEta(pJetEta);
        ttPython::Py_buffer_wrapper<Float_t> jetPhi(pJetPhi);
        ttPython::Py_buffer_wrapper<Float_t> jetM(pJetMass);

        std::vector<TLorentzVector> jetsLV(jetPt.size());
        for(unsigned int iJet = 0; iJet < jetPt.size(); ++iJet)
        {
            jetsLV[iJet].SetPtEtaPhiM(jetPt[iJet], jetEta[iJet], jetPhi[iJet], jetM[iJet]);
        }

        //prepare b-tag discriminator
        ttPython::Py_buffer_wrapper<Float_t> jetBTag(pJetBtag);

        //Create the AK4 constituent helper
        ttUtility::ConstAK4Inputs<Float_t, ttPython::Py_buffer_wrapper<Float_t>> ak4ConstInputs(jetsLV, jetBTag);

        //prepare floating point supplamental "vectors"
        PyObject *key, *value;
        Py_ssize_t pos = 0;

        std::vector<ttPython::Py_buffer_wrapper<Float_t>> tempFloatBuffers;
        //reserve space for the vector to stop reallocations during emplacing, need space for float and int "vectors" stored here
        Py_ssize_t floatSize = PyDict_Size(pFloatVarsDict);
        Py_ssize_t intSize = PyDict_Size(pIntVarsDict);
        tempFloatBuffers.reserve(floatSize + intSize);

        while(PyDict_Next(pFloatVarsDict, &pos, &key, &value)) 
        {
            if(PyString_Check(key))
            {
                //Get the ROOT.PyFloatBuffer into a c++ friendly format
                tempFloatBuffers.emplace_back(value);

                char *vecName = PyString_AsString(key);
                ak4ConstInputs.addSupplamentalVector(vecName, tempFloatBuffers.back());
            }
            else
            {
                //Handle error here
                Py_DECREF(ptt);
                Py_DECREF(pJetPt);
                Py_DECREF(pJetEta);
                Py_DECREF(pJetPhi);
                Py_DECREF(pJetMass);
                Py_DECREF(pJetBtag);
                Py_DECREF(pFloatVarsDict);
                Py_DECREF(pIntVarsDict);

                PyErr_SetString(PyExc_KeyError, "Dictionary keys must be strings for top tagger supplamentary variables.");
                return NULL;
            }
        }

        //prepare integer supplamental "vectors" and convert to float vector 
        pos = 0;
        std::vector<std::vector<Float_t>> tempIntToFloatVectors;
        //reserve space for the vector to stop reallocations during emplacing 
        tempIntToFloatVectors.reserve(intSize);
        while(PyDict_Next(pIntVarsDict, &pos, &key, &value)) 
        {
            if(PyString_Check(key))
            {
                //Get the ROOT.PyIntBuffer into a c++ friendly format
                ttPython::Py_buffer_wrapper<Int_t> buffer(value);
            
                //translate the integers to floats
                tempIntToFloatVectors.emplace_back(buffer.begin(), buffer.end());

                //wrap vector in buffer
                tempFloatBuffers.emplace_back(&(tempIntToFloatVectors.back()));
            
                char *vecName = PyString_AsString(key);
                ak4ConstInputs.addSupplamentalVector(vecName, tempFloatBuffers.back());
            }
            else
            {
                //Handle error here
                Py_DECREF(ptt);
                Py_DECREF(pJetPt);
                Py_DECREF(pJetEta);
                Py_DECREF(pJetPhi);
                Py_DECREF(pJetMass);
                Py_DECREF(pJetBtag);
                Py_DECREF(pFloatVarsDict);
                Py_DECREF(pIntVarsDict);

                PyErr_SetString(PyExc_KeyError, "Dictionary keys must be strings for top tagger supplamentary variables.");
                return NULL;
            }
        }

        //Get top tagger pointer from capsule 
        TopTagger* tt;
        if (!(tt = (TopTagger*) PyCapsule_GetPointer(ptt, "TopTagger"))) 
        {
            //Handle exception here 
            Py_DECREF(ptt);
            Py_DECREF(pJetPt);
            Py_DECREF(pJetEta);
            Py_DECREF(pJetPhi);
            Py_DECREF(pJetMass);
            Py_DECREF(pJetBtag);
            Py_DECREF(pFloatVarsDict);
            Py_DECREF(pIntVarsDict);

            PyErr_SetString(PyExc_ReferenceError, "TopTagger pointer invalid");
            return NULL;
        }

        //Run top tagger
        try
        {
            //Create constituents 
            const auto& constituents = ttUtility::packageConstituents(ak4ConstInputs);

            //Run top tagger 
            tt->runTagger(constituents);
        }
        catch(const TTException& e)
        {
            Py_DECREF(ptt);
            Py_DECREF(pJetPt);
            Py_DECREF(pJetEta);
            Py_DECREF(pJetPhi);
            Py_DECREF(pJetMass);
            Py_DECREF(pJetBtag);
            Py_DECREF(pFloatVarsDict);
            Py_DECREF(pIntVarsDict);

            std::cout << "TopTagger exception message: " << e << std::endl;
            PyErr_SetString(PyExc_RuntimeError, "TopTagger exception thrown (look above to find specific exception message)");
            return NULL;
        }

        Py_DECREF(ptt);
        Py_DECREF(pJetPt);
        Py_DECREF(pJetEta);
        Py_DECREF(pJetPhi);
        Py_DECREF(pJetMass);
        Py_DECREF(pJetBtag);
        Py_DECREF(pFloatVarsDict);
        Py_DECREF(pIntVarsDict);

        Py_INCREF(Py_None);
        return Py_None;
    }

    static PyObject* TopTaggerInterface_getResults(PyObject *self, PyObject *args)
    {
        //suppress unused parameter warning as self is manditory
        (void)self;

        PyObject *ptt;
        if (!PyArg_ParseTuple(args, "O!", &PyCapsule_Type, &ptt))
        {
            PyErr_SetString(PyExc_TypeError, "Incorrect function parameters");
            return NULL;
        }

        Py_INCREF(ptt);

        //Get top tagger pointer from capsule 
        TopTagger* tt;
        if (!(tt = (TopTagger*) PyCapsule_GetPointer(ptt, "TopTagger"))) 
        {
            //Handle exception here 
            Py_DECREF(ptt);

            PyErr_SetString(PyExc_ReferenceError, "TopTagger pointer invalid");
            return NULL;
        }

        try
        {
            //Get top tagger results 
            const auto& ttr = tt->getResults();

            //Get tops 
            const auto& tops = ttr.getTops();

            //create numpy arrays for passing top data to python
            const npy_intp NVARSFLOAT = 5;
            const npy_intp NVARSINT = 4;
            const npy_intp NTOPS = static_cast<npy_intp>(tops.size());
    
            npy_intp floatsizearray[] = {NTOPS, NVARSFLOAT};
            PyArrayObject* topArrayFloat = reinterpret_cast<PyArrayObject*>(PyArray_SimpleNew(2, floatsizearray, NPY_FLOAT));

            npy_intp intsizearray[] = {NTOPS, NVARSINT};
            PyArrayObject* topArrayInt = reinterpret_cast<PyArrayObject*>(PyArray_SimpleNew(2, intsizearray, NPY_INT));

            //fill numpy array
            for(unsigned int iTop = 0; iTop < tops.size(); ++iTop)
            {
                *static_cast<npy_float*>(PyArray_GETPTR2(topArrayFloat, iTop, 0)) = tops[iTop]->p().Pt();
                *static_cast<npy_float*>(PyArray_GETPTR2(topArrayFloat, iTop, 1)) = tops[iTop]->p().Eta();
                *static_cast<npy_float*>(PyArray_GETPTR2(topArrayFloat, iTop, 2)) = tops[iTop]->p().Phi();
                *static_cast<npy_float*>(PyArray_GETPTR2(topArrayFloat, iTop, 3)) = tops[iTop]->p().M();
                *static_cast<npy_float*>(PyArray_GETPTR2(topArrayFloat, iTop, 4)) = tops[iTop]->getDiscriminator();

                *static_cast<npy_int*>(PyArray_GETPTR2(topArrayInt, iTop, 0)) = static_cast<int>(tops[iTop]->getType());

                //get constituents vector to retrieve matching index
                const auto& topConstituents = tops[iTop]->getConstituents();
                if(topConstituents.size() > 0) *static_cast<npy_int*>(PyArray_GETPTR2(topArrayInt, iTop, 1)) = static_cast<int>(topConstituents[0]->getIndex());
                else                                  *static_cast<npy_int*>(PyArray_GETPTR2(topArrayInt, iTop, 1)) = -1;

                if(topConstituents.size() > 1) *static_cast<npy_int*>(PyArray_GETPTR2(topArrayInt, iTop, 2)) = static_cast<int>(topConstituents[1]->getIndex());
                else                                  *static_cast<npy_int*>(PyArray_GETPTR2(topArrayInt, iTop, 2)) = -1;

                if(topConstituents.size() > 2) *static_cast<npy_int*>(PyArray_GETPTR2(topArrayInt, iTop, 3)) = static_cast<int>(topConstituents[2]->getIndex());
                else                                  *static_cast<npy_int*>(PyArray_GETPTR2(topArrayInt, iTop, 3)) = -1;
            }

            Py_DECREF(ptt);

            return Py_BuildValue("NN", topArrayFloat, topArrayInt);
        }
        catch(const TTException& e)
        {
            Py_DECREF(ptt);

            std::cout << "TopTagger exception message: " << e << std::endl;
            PyErr_SetString(PyExc_RuntimeError, "TopTagger exception thrown (look above to find specific exception message)");
            return NULL;
        }

    }

    static PyMethodDef TopTaggerInterfaceMethods[] = {
        {"setup",  TopTaggerInterface_setup, METH_VARARGS, "Configure Top Tagger."},
        {"run",  TopTaggerInterface_run, METH_VARARGS, "Run Top Tagger."},
        {"getResults",  TopTaggerInterface_getResults, METH_VARARGS, "Get Top Tagger results."},
        {NULL, NULL, 0, NULL}        /* Sentinel */
    };

    PyMODINIT_FUNC
    initTopTaggerInterface(void)
    {
        (void) Py_InitModule("TopTaggerInterface", TopTaggerInterfaceMethods);

        //Setup numpy
        import_array();
    }

}

#endif