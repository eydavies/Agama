/** \file   py_wrapper.cpp
    \brief  Python wrapper for the library
    \author Eugene Vasiliev
    \date   2014-2015
*/
#include <Python.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
//#define DEBUGPRINT
#include <numpy/arrayobject.h>
#include "units.h"
#include "potential_factory.h"
#include "potential_composite.h"
#include "actions_staeckel.h"
#include "actions_torus.h"
#include "math_spline.h"

/// \name  Some general definitions
///@{

/// arguments of functions
struct ArgDescription {
    const char* name;  ///< argument name
    char type;         ///< letter that determines argument type ('s','d','i','O')
    const char* descr; ///< textual description
};

/// argument type in human-readable words
static const char* argTypeDescr(char type)
{
    switch(type) {
        case 's': return "string";
        case 'i': return "int";
        case 'd': return "float";
        case 'O': return "object";
        default:  return "unknown type";
    }
}

/// max number of keywords
static const size_t MAX_NUM_KEYWORDS = 64;

/// max size of docstring
static const size_t MAX_LEN_DOCSTRING = 4096;

///@}
/// \name  ------- Unit handling routines --------
///@{

/// internal working units
static const units::InternalUnits unit(units::Kpc, units::Myr);

/// external units that are used in the calling code
static const units::ExternalUnits* conv;

/// description of set_units function
static const char* docstringUnits = 
    "Inform the library about the physical units that are used in Python code\n"
    "Arguments should be any three independent physical quantities that define "
    "'mass', 'length', 'velocity' or 'time' scales "
    "(note that the latter three are not all independent).\n"
    "Their values specify the units in terms of "
    "'Solar mass', 'Kiloparsec', 'km/s' and 'Megayear', correspondingly.\n"
    "Example: standard GADGET units are defined as\n"
    "    setUnits(mass=1e10, length=1, velocity=1)\n";

/// define the unit conversion
static PyObject* set_units(PyObject* /*self*/, PyObject* args, PyObject* namedArgs)
{
    static const char* keywords[] = {"mass", "length", "velocity", "time", NULL};
    double mass = 0, length = 0, velocity = 0, time = 0;
    if(!PyArg_ParseTupleAndKeywords(
        args, namedArgs, "|dddd", const_cast<char**>(keywords),
        &mass, &length, &velocity, &time) ||
        mass<0 || length<0 || velocity<0 || time<0)
    {
        PyErr_SetString(PyExc_ValueError, "Invalid arguments passed to setUnits()");
        return NULL;
    }
    if(length>0 && velocity>0 && time>0) {
        PyErr_SetString(PyExc_ValueError, 
            "You may not assign length, velocity and time units simultaneously");
        return NULL;
    }
    if(mass==0) {
        PyErr_SetString(PyExc_ValueError, "You must specify mass unit");
        return NULL;
    }
    const units::ExternalUnits* newConv = NULL;
    if(length>0 && time>0)
        newConv = new units::ExternalUnits(unit,
            length*units::Kpc, length/time * units::Kpc/units::Myr, mass*units::Msun);
    else if(length>0 && velocity>0)
        newConv = new units::ExternalUnits(unit,
            length*units::Kpc, velocity*units::kms, mass*units::Msun);
    else if(time>0 && velocity>0)
        newConv = new units::ExternalUnits(unit,
            velocity*time * units::kms*units::Myr, velocity*units::kms, mass*units::Msun);
    else {
        PyErr_SetString(PyExc_ValueError,
            "You must specify exactly two out of three units: length, time and velocity");
        return NULL;
    }
    delete conv;
    conv = newConv;
    Py_INCREF(Py_None);
    return Py_None;
}

///@}
/// \name ----- a truly general interface for evaluating some function 
///             for some input data and storing its output somewhere -----
///@{

/// any function that evaluates something for a given object and an `input` array of floats,
/// and stores one or more values in the `result` array of floats
typedef void (*anyFunction) 
    (PyObject* obj, const double input[], double *result);

/// anyFunction input type
enum INPUT_VALUE {
    INPUT_VALUE_SINGLE = 1,  ///< a single number
    INPUT_VALUE_TRIPLET= 3,  ///< three numbers
    INPUT_VALUE_SEXTET = 6   ///< six numbers   
};

/// anyFunction output type; numerical value is arbitrary
enum OUTPUT_VALUE {
    OUTPUT_VALUE_SINGLE              = 1,  ///< scalar value
    OUTPUT_VALUE_TRIPLET             = 3,  ///< a triplet of numbers
    OUTPUT_VALUE_SEXTET              = 6,  ///< a sextet of numbers
    OUTPUT_VALUE_TRIPLET_AND_TRIPLET = 33, ///< a triplet and another triplet -- two separate arrays
    OUTPUT_VALUE_TRIPLET_AND_SEXTET  = 36  ///< a triplet and a sextet
};

/// size of input array for a single point
template<int numArgs>
static size_t inputLength();

/// parse a list of numArgs floating-point arguments for a Python function, and store them in inputArray[]
template<int numArgs>
int parseTuple(PyObject* args, double inputArray[]);

/// error message for an input array of incorrect size
template<int numArgs>
const char* errStrInvalidArrayDim();

/// error message for an input array of incorrect size or an invalid list of arguments
template<int numArgs>
const char* errStrInvalidInput();

/// size of output array for a single point
template<int numOutput>
static size_t outputLength();

/// construct an output tuple containing the given result data computed for a single input point
template<int numOutput>
PyObject* formatTuple(const double result[]);

/// construct an output array, or several arrays, that will store the output for many input points
template<int numOutput>
PyObject* allocOutputArr(int size);

/// store the 'result' data computed for a single input point in an output array 'resultObj' at 'index'
template<int numOutput>
void formatOutputArr(const double result[], const int index, PyObject* resultObj);

// ---- template instantiations for input parameters ----

template<> static size_t inputLength<INPUT_VALUE_SINGLE>()  {return 1;}
template<> static size_t inputLength<INPUT_VALUE_TRIPLET>() {return 3;}
template<> static size_t inputLength<INPUT_VALUE_SEXTET>()  {return 6;}

template<> int parseTuple<INPUT_VALUE_TRIPLET>(PyObject* args, double input[]) {
    return PyArg_ParseTuple(args, "ddd", &input[0], &input[1], &input[2]);
}
template<> int parseTuple<INPUT_VALUE_SEXTET>(PyObject* args, double input[]) {
    return PyArg_ParseTuple(args, "dddddd",
        &input[0], &input[1], &input[2], &input[3], &input[4], &input[5]);
}

template<> const char* errStrInvalidArrayDim<INPUT_VALUE_TRIPLET>() {
    return "Input does not contain valid Nx3 array";
}
template<> const char* errStrInvalidArrayDim<INPUT_VALUE_SEXTET>() {
    return "Input does not contain valid Nx6 array";
}

template<> const char* errStrInvalidInput<INPUT_VALUE_TRIPLET>() {
    return "Input does not contain valid data (either 3 numbers for a single point or a Nx3 array)";
}
template<> const char* errStrInvalidInput<INPUT_VALUE_SEXTET>() {
    return "Input does not contain valid data (either 6 numbers for a single point or a Nx6 array)";
}

// ---- template instantiations for output parameters ----

template<> static size_t outputLength<OUTPUT_VALUE_SINGLE>()  {return 1;}
template<> static size_t outputLength<OUTPUT_VALUE_TRIPLET>() {return 3;}
template<> static size_t outputLength<OUTPUT_VALUE_SEXTET>()  {return 6;}
template<> static size_t outputLength<OUTPUT_VALUE_TRIPLET_AND_TRIPLET>() {return 6;}
template<> static size_t outputLength<OUTPUT_VALUE_TRIPLET_AND_SEXTET>()  {return 9;}

template<> PyObject* formatTuple<OUTPUT_VALUE_SINGLE>(const double result[]) {
    return Py_BuildValue("d", result[0]);
}
template<> PyObject* formatTuple<OUTPUT_VALUE_TRIPLET>(const double result[]) {
    return Py_BuildValue("ddd", result[0], result[1], result[2]);
}
template<> PyObject* formatTuple<OUTPUT_VALUE_TRIPLET_AND_SEXTET>(const double result[]) {
    return Py_BuildValue("(ddd)(dddddd)", result[0], result[1], result[2],
        result[3], result[4], result[5], result[6], result[7], result[8]);
}

template<> PyObject* allocOutputArr<OUTPUT_VALUE_SINGLE>(int size) {
    npy_intp dims[] = {size};
    return PyArray_SimpleNew(1, dims, NPY_DOUBLE);
}
template<> PyObject* allocOutputArr<OUTPUT_VALUE_TRIPLET>(int size) {
    npy_intp dims[] = {size, 3};
    return PyArray_SimpleNew(2, dims, NPY_DOUBLE);
}
template<> PyObject* allocOutputArr<OUTPUT_VALUE_TRIPLET_AND_SEXTET>(int size) {
    npy_intp dims1[] = {size, 3};
    npy_intp dims2[] = {size, 6};
    PyObject* arr1 = PyArray_SimpleNew(2, dims1, NPY_DOUBLE);
    PyObject* arr2 = PyArray_SimpleNew(2, dims2, NPY_DOUBLE);
    return Py_BuildValue("NN", arr1, arr2);
}

template<> void formatOutputArr<OUTPUT_VALUE_SINGLE>(
    const double result[], const int index, PyObject* resultObj) 
{
    ((double*)PyArray_DATA((PyArrayObject*)resultObj))[index] = result[0];
}
template<> void formatOutputArr<OUTPUT_VALUE_TRIPLET>(
    const double result[], const int index, PyObject* resultObj) 
{
    for(int d=0; d<3; d++)
        ((double*)PyArray_DATA((PyArrayObject*)resultObj))[index*3+d] = result[d];
}
template<> void formatOutputArr<OUTPUT_VALUE_TRIPLET_AND_SEXTET>(
    const double result[], const int index, PyObject* resultObj) 
{
    PyArrayObject* arr1 = (PyArrayObject*) PyTuple_GET_ITEM(resultObj, 0);
    PyArrayObject* arr2 = (PyArrayObject*) PyTuple_GET_ITEM(resultObj, 1);
    for(int d=0; d<3; d++)
        ((double*)PyArray_DATA(arr1))[index*3+d] = result[d];
    for(int d=0; d<6; d++)
        ((double*)PyArray_DATA(arr2))[index*6+d] = result[d+3];
}

/** A general function that computes something for one or many input points.
    \param[in]  fnc  is the function pointer to the routine that actually computes something,
    taking a pointer to an instance of Python object, an array of floats as the input point,
    and producing another array of floats as the output.
    \tparam numArgs  is the size of array that contains the value of a single input point.
    \tparam numOutput is the identifier (not literally the size) of output data format 
    for a single input point: it may be a single number, an array of floats, or even several arrays.
    \param[in] self  is the pointer to Python object that is passed to the 'fnc' routine
    \param[in] args  is the arguments of the function call: it may be a sequence of numArg floats 
    that represents a single input point, or a 1d array of the same length and same meaning,
    or a 2d array of dimensions N * numArgs, representing N input points.
    \returns  the result of applying 'fnc' to one or many input points, in the form determined 
    both by the number of input points, and the output data format. 
    The output for a single point may be a sequence of numbers (tuple or 1d array), 
    or several such arrays forming a tuple (e.g., [ [1,2,3], [1,2,3,4,5,6] ]). 
    The output for an array of input points would be one or several 2d arrays of length N and 
    shape determined by the output format, i.e., for the above example it would be ([N,3], [N,6]).
*/
template<int numArgs, int numOutput>
static PyObject* callAnyFunctionOnArray(PyObject* self, PyObject* args, anyFunction fnc)
{
    double input [inputLength<numArgs>()];
    double result[outputLength<numOutput>()];
    try{
        if(parseTuple<numArgs>(args, input)) {  // one point
            fnc(self, input, result);
            return formatTuple<numOutput>(result);
        }
        PyErr_Clear();  // clear error if the argument list is not a tuple of a proper type
        PyObject* obj;
        if(PyArg_ParseTuple(args, "O", &obj)) {
            PyArrayObject *arr  = (PyArrayObject*) PyArray_FROM_OTF(obj,  NPY_DOUBLE, NPY_ARRAY_IN_ARRAY);
            if(arr == NULL) {
                PyErr_SetString(PyExc_ValueError, "Input does not contain a valid array");
                return NULL;
            }
            int numpt = 0;
            if(PyArray_NDIM(arr) == 1 && PyArray_DIM(arr, 0) == numArgs) 
            {   // 1d array of length numArgs - a single point
                fnc(self, static_cast<double*>(PyArray_GETPTR1(arr, 0)), result);
                Py_DECREF(arr);
                return formatTuple<numOutput>(result);
            }
            if(PyArray_NDIM(arr) == 2 && PyArray_DIM(arr, 1) == numArgs)
                numpt = PyArray_DIM(arr, 0);
            else {
                PyErr_SetString(PyExc_ValueError, errStrInvalidArrayDim<numArgs>());
                Py_DECREF(arr);
                return NULL;
            }
            // allocate an appropriate output object
            PyObject* resultObj = allocOutputArr<numOutput>(numpt);
            // loop over input array
            for(int i=0; i<numpt; i++) {
                fnc(self, static_cast<double*>(PyArray_GETPTR2(arr, i, 0)), result);
                formatOutputArr<numOutput>(result, i, resultObj);
            }                
            Py_DECREF(arr);
            return resultObj;
        }
        PyErr_SetString(PyExc_ValueError, errStrInvalidInput<numArgs>());
        return NULL;
    }
    catch(std::exception& e) {
        PyErr_SetString(PyExc_ValueError, (std::string("Exception occured: ")+e.what()).c_str());
        return NULL;
    }
}

///@}
/// \name  ---------- Potential class and related data ------------
///@{

/// Python type corresponding to Potential class
typedef struct {
    PyObject_HEAD
    const potential::BasePotential* pot;
} PotentialObject;

static PyObject* Potential_new(PyTypeObject *type, PyObject*, PyObject*)
{
    PotentialObject *self = (PotentialObject*)type->tp_alloc(type, 0);
    if(self)
        self->pot=NULL;
    return (PyObject*)self;
}

static void Potential_dealloc(PotentialObject* self)
{
    if(self->pot) {
#ifdef DEBUGPRINT
        printf("Deleted an instance of %s potential\n", self->pot->name());
#endif
        delete self->pot;
    }
#ifdef DEBUGPRINT
    else printf("Deleted empty Potential\n");
#endif
    self->ob_type->tp_free((PyObject*)self);
}

/// pointer to the Potential type object (will be initialized below)
static PyTypeObject* PotentialTypePtr;

/// list of all possible arguments of Potential class constructor
static const ArgDescription potentialArgs[] = {
    {"file",              's', "the name of ini file, potential coefficients file, or N-body snapshot file"},
    {"type",              's', "potential type, such as 'Plummer', 'Ferrers', or potential expansion type, such as 'SplineExp'"},
    {"density",           's', "density model for potential expansion, like 'Dehnen', 'MGE'"},
    {"symmetry",          's', "assumed symmetry for potential expansion constructed from an N-body snapshot"},
    {"points",            'O', "array of point masses to be used in construction of a potential expansion: "
        "should be a tuple with two arrays - coordinates and mass, where the first one "
        "is a two-dimensional Nx3 array and the second one is a one-dimensional array of length N"},
    {"mass",              'd', "total mass of the model"},
    {"scaleradius",       'd', "scale radius of the model (if applicable)"},
    {"scaleradius2",      'd', "second scale radius of the model (if applicable)"},
    {"q",                 'd', "axis ratio y/x, i.e., intermediate to long axis (if applicable)"},
    {"p",                 'd', "axis ratio z/x, i.e., short to long axis (if applicable)"},
    {"gamma",             'd', "central cusp slope (applicable for Dehnen model)"},
    {"sersicindex",       'd', "Sersic index (applicable for Sersic density model)"},
    {"numcoefsradial",    'i', "number of radial terms in BasisSetExp or grid points in spline potentials"},
    {"numcoefsangular",   'i', "order of spherical-harmonic expansion (max.index of angular harmonic coefficient)"},
    {"numcoefsvertical",  'i', "number of coefficients in z-direction for CylSplineExp potential"},
    {"alpha",             'd', "parameter that determines functional form of BasisSetExp potential"},
    {"splinesmoothfactor",'d', "amount of smoothing in SplineExp initialized from an N-body snapshot"},
    {"splinermin",        'd', "if nonzero, specifies the innermost grid node radius for SplineExp and CylSplineExp"},
    {"splinermax",        'd', "if nonzero, specifies the outermost grid node radius for SplineExp and CylSplineExp"},
    {"splinezmin",        'd', "if nonzero, specifies the z-value of the innermost grid node in CylSplineExp"},
    {"splinezmax",        'd', "if nonzero, specifies the z-value of the outermost grid node in CylSplineExp"},
    {NULL,0,NULL}
};

/// this string will contain full list of parameters and other relevant info 
/// to be printed out via `help(Potential)`; it is filled during module initialization
static char docstringPotential[MAX_LEN_DOCSTRING];

/// list of keywords extracted from potentialArgs 
static const char* keywordsPotential[MAX_NUM_KEYWORDS] = {NULL};

/// list of keyword types extracted from potentialArgs
static char keywordTypesPotential[MAX_NUM_KEYWORDS+1] = "|";

/// build the docstring for Potential class
static void buildDocstringPotential()
{
    // header line, store the number of symbols printed
    unsigned int bufIndex = snprintf(docstringPotential, MAX_LEN_DOCSTRING,
        "Potential is a class that represents a wide range of gravitational potentials\n"
        "There are a number of possible named arguments for the constructor:\n\n");
    unsigned int argIndex = 0;
    while(potentialArgs[argIndex].name != NULL && bufIndex < MAX_LEN_DOCSTRING && argIndex < MAX_NUM_KEYWORDS) {
        bufIndex += snprintf(docstringPotential + bufIndex, MAX_LEN_DOCSTRING - bufIndex,
            "    %s (%s) - %s\n", potentialArgs[argIndex].name,
            argTypeDescr(potentialArgs[argIndex].type), potentialArgs[argIndex].descr);
        keywordsPotential[argIndex] = potentialArgs[argIndex].name;
        keywordTypesPotential[argIndex+1] = potentialArgs[argIndex].type;
        argIndex++;
    }
    if(bufIndex < MAX_LEN_DOCSTRING) {
        bufIndex += snprintf(docstringPotential + bufIndex, MAX_LEN_DOCSTRING - bufIndex,
            "\nRequired parameters are either 'type' or 'file' (or both)\n"
            "Alternatively, a composite potential may be created by passing a tuple "
            "of Potential objects as the argument list for the constructor; "
            "NOTE that these components will no longer be usable after being incorporated "
            "into the composite potential!");
    }
    if(bufIndex >= MAX_LEN_DOCSTRING || argIndex >= MAX_NUM_KEYWORDS)
    {   // overflow shouldn't occur, but if it does, issue a warning
        printf("WARNING: Could not properly initialize Potential class docstring\n");
    }
}

/// finalize the call to constructor by assigning the member variable 'pot'
static void Potential_initMemberVar(PyObject* self, const potential::BasePotential* pot)
{
#ifdef DEBUGPRINT
    printf("Created an instance of %s potential\n", pot->name());
#endif
    if(((PotentialObject*)self)->pot)
    {  // check if this is not the first time that constructor is called
#ifdef DEBUGPRINT
        printf("Deleted previous instance of %s potential\n", ((PotentialObject*)self)->pot->name());
#endif
        delete ((PotentialObject*)self)->pot;
    }
    ((PotentialObject*)self)->pot = pot;
}

/// attempt to construct a composite potential from a tuple of Potential objects
static int Potential_initComposite(PyObject* self, PyObject* args)
{
    if(PyTuple_CheckExact(args) && PyTuple_Size(args)>0) {
        bool correct = true;
        for(Py_ssize_t i=0; correct && i<PyTuple_Size(args); i++) 
            correct &= PyObject_TypeCheck(PyTuple_GET_ITEM(args, i), PotentialTypePtr);
        if(!correct) {
            PyErr_SetString(PyExc_ValueError, 
                "The arguments of constructor are not a tuple of Potential objects");
            return -1;
        }
        /* in the present implementation, creating a composite potential from an array 
           of BasePotential* pointers means that they are "taken over" by the new composite
           potential and will be deleted when the latter is destroyed. 
           Since we must not delete the same object twice, we make sure that the original 
           Potential objects in Python will no longer be usable after creating a composite, 
           but they won't be automatically deleted until their refcounter drops to zero.
        */
        std::vector<const potential::BasePotential*> components;
        for(Py_ssize_t i=0; i<PyTuple_Size(args); i++) {
            components.push_back(((PotentialObject*)PyTuple_GET_ITEM(args, i))->pot);
            ((PotentialObject*)PyTuple_GET_ITEM(args, i))->pot = NULL;  // won't be usable anymore
        }
        try{
            const potential::BasePotential* pot = new potential::CompositeCyl(components);
            Potential_initMemberVar(self, pot);
            return 0;
        }
        catch(std::exception& e) {
            PyErr_SetString(PyExc_ValueError, 
                (std::string("Error in creating composite potential: ")+e.what()).c_str());
            return -1;
        }
    }
    return -1;  // not successful
}

/// construct potential::BasePotential* object from particles
static int Potential_initFromParticles(PyObject* self, 
    const potential::ConfigPotential& cfg, PyObject* points)
{
    if( !cfg.fileName.empty() ) {
        PyErr_SetString(PyExc_ValueError, "Cannot provide both points and filename");
        return -1;
    }
    if( cfg.potentialType != potential::PT_BSE &&
        cfg.potentialType != potential::PT_SPLINE &&
        cfg.potentialType != potential::PT_CYLSPLINE ) {
        PyErr_SetString(PyExc_ValueError, "Potential should be of an expansion type");
        return -1;
    }
    PyObject *pointCoordObj, *pointMassObj;
    if(!PyArg_ParseTuple(points, "OO", &pointCoordObj, &pointMassObj)) {
        PyErr_SetString(PyExc_ValueError, "'points' must be a tuple with two arrays - "
            "coordinates and mass, where the first one is a two-dimensional Nx3 array "
            "and the second one is a one-dimensional array of length N");
        return -1;
    }
    PyArrayObject *pointCoordArr = (PyArrayObject*)
        PyArray_FROM_OTF(pointCoordObj, NPY_DOUBLE, NPY_ARRAY_IN_ARRAY);
    PyArrayObject *pointMassArr  = (PyArrayObject*)
        PyArray_FROM_OTF(pointMassObj,  NPY_DOUBLE, NPY_ARRAY_IN_ARRAY);
    if(pointCoordArr == NULL || pointMassArr == NULL) {
        Py_XDECREF(pointCoordArr);
        Py_XDECREF(pointMassArr);
        PyErr_SetString(PyExc_ValueError, "'points' does not contain valid arrays");
        return -1;
    }
    int numpt = 0;
    if(PyArray_NDIM(pointMassArr) == 1)
        numpt = PyArray_DIM(pointMassArr, 0);
    if(numpt == 0 || PyArray_NDIM(pointCoordArr) != 2 || 
        PyArray_DIM(pointCoordArr, 0) != numpt || PyArray_DIM(pointCoordArr, 1) != 3)
    {
        Py_DECREF(pointCoordArr);
        Py_DECREF(pointMassArr);
        PyErr_SetString(PyExc_ValueError, "'points' does not contain valid arrays "
            "(the first one must be 2d array of shape Nx3 and the second one must be 1d array of length N)");
        return -1;
    }
    try{
        particles::PointMassArray<coord::PosCar> pointArray;
        pointArray.data.reserve(numpt);
        for(int i=0; i<numpt; i++) {
            double x = *static_cast<double*>(PyArray_GETPTR2(pointCoordArr, i, 0)) * conv->lengthUnit;
            double y = *static_cast<double*>(PyArray_GETPTR2(pointCoordArr, i, 1)) * conv->lengthUnit;
            double z = *static_cast<double*>(PyArray_GETPTR2(pointCoordArr, i, 2)) * conv->lengthUnit;
            double m = *static_cast<double*>(PyArray_GETPTR1(pointMassArr, i)) * conv->massUnit;
            pointArray.add(coord::PosCar(x,y,z), m);
        }
        const potential::BasePotential* pot = potential::createPotentialFromPoints(cfg, pointArray);
        assert(pot!=NULL);
        Potential_initMemberVar(self, pot);
        Py_DECREF(pointCoordArr);
        Py_DECREF(pointMassArr);
        return 0;
    }
    catch(std::exception& e) {
        PyErr_SetString(PyExc_ValueError, 
            (std::string("Error in creating potential from points: ")+e.what()).c_str());
        Py_DECREF(pointCoordArr);
        Py_DECREF(pointMassArr);
        return -1;
    }
}

/// the generic constructor of Potential object
static int Potential_init(PyObject* self, PyObject* args, PyObject* namedArgs)
{    
    // check if the input was a tuple of Potential objects, and if so, return a composite potential
    if(Potential_initComposite(self, args) == 0)
        return 0;
    potential::ConfigPotential cfg;
    const char* file="";
    const char* type="";
    const char* density="";
    const char* symmetry="";
    PyObject* points=NULL;

    // it is VITALLY IMPORTANT to list all data fields in exactly the same order
    // as appeared in the `potentialArgs` array!!!
    if(!PyArg_ParseTupleAndKeywords(args, namedArgs, keywordTypesPotential, const_cast<char**>(keywordsPotential),
        &file, &type, &density, &symmetry,
        &points,
        &(cfg.mass), &(cfg.scaleRadius), &(cfg.scaleRadius2), &(cfg.q), &(cfg.p), &(cfg.gamma), &(cfg.sersicIndex),
        &(cfg.numCoefsRadial), &(cfg.numCoefsAngular), &(cfg.numCoefsVertical), 
        &(cfg.alpha), &(cfg.splineSmoothFactor), &(cfg.splineRMin), &(cfg.splineRMax), &(cfg.splineZMin), &(cfg.splineZMax) ))
    {
        PyErr_SetString(PyExc_ValueError, "Invalid arguments passed to the Potential constructor;\n"
            "type 'help(Potential)' to get the list of possible arguments and their types");
        return -1;
    }
    cfg.fileName      = std::string(file);
    cfg.potentialType = potential::getPotentialTypeByName(type);
    cfg.densityType   = potential::getDensityTypeByName(density);
    cfg.symmetryType  = potential::getSymmetryTypeByName(symmetry);
    cfg.mass         *= conv->massUnit;
    cfg.scaleRadius  *= conv->lengthUnit;
    cfg.scaleRadius2 *= conv->lengthUnit;
    cfg.splineRMin   *= conv->lengthUnit;
    cfg.splineRMax   *= conv->lengthUnit;
    cfg.splineZMin   *= conv->lengthUnit;
    cfg.splineZMax   *= conv->lengthUnit;
    try{  // the code below may generate exceptions which shouldn't propagate to Python
        if(points!=NULL) {  // a list of particles was provided
            return Potential_initFromParticles(self, cfg, points);
        } else {   // attempt to create potential from configuration parameters
            if(type[0]==0 && file[0]==0) {
                PyErr_SetString(PyExc_ValueError, "Should provide type='...' or file='...' parameter");
                return -1;
            }
            const potential::BasePotential* pot = potential::createPotential(cfg);
            assert(pot!=NULL);
            Potential_initMemberVar(self, pot);
            return 0;
        }
    }
    catch(std::exception& e) {
        PyErr_SetString(PyExc_ValueError, (std::string("Error in creating potential: ")+e.what()).c_str());
        return -1;
    }
}

static bool Potential_isCorrect(PyObject* self)
{
    if(self==NULL) {
        PyErr_SetString(PyExc_ValueError, "Should be called as method of Potential object");
        return false;
    }
    if(((PotentialObject*)self)->pot==NULL) {
        PyErr_SetString(PyExc_ValueError, "Potential is not initialized properly");
        return false;
    }
    return true;
}

// function that do actually compute something from the potential object,
// applying appropriate unit conversions

static void fncPotential(PyObject* obj, const double input[], double *result) {
    const coord::PosCar point(
        input[0] * conv->lengthUnit, 
        input[1] * conv->lengthUnit, 
        input[2] * conv->lengthUnit);
    result[0] =
        ((PotentialObject*)obj)->pot->value(point)
        / pow_2(conv->velocityUnit);   // unit of potential is V^2
}
static PyObject* Potential_potential(PyObject* self, PyObject* args) {
    if(!Potential_isCorrect(self))
        return NULL;
    return callAnyFunctionOnArray<INPUT_VALUE_TRIPLET, OUTPUT_VALUE_SINGLE>
        (self, args, fncPotential);
}
static PyObject* Potential_value(PyObject* self, PyObject* args, PyObject* /*namedargs*/) {
    if(!Potential_isCorrect(self))
        return NULL;
    return Potential_potential(self, args);
}

static void fncDensity(PyObject* obj, const double input[], double *result) {
    const coord::PosCar point(
        input[0] * conv->lengthUnit, 
        input[1] * conv->lengthUnit, 
        input[2] * conv->lengthUnit);
    result[0] =
        ((PotentialObject*)obj)->pot->density(point)
        / (conv->massUnit / pow_2(conv->lengthUnit));  // unit of density is M/L^3
}
static PyObject* Potential_density(PyObject* self, PyObject* args) {
    if(!Potential_isCorrect(self))
        return NULL;
    return callAnyFunctionOnArray<INPUT_VALUE_TRIPLET, OUTPUT_VALUE_SINGLE>
        (self, args, fncDensity);
}

static void fncForce(PyObject* obj, const double input[], double *result) {
    const coord::PosCar point(
        input[0] * conv->lengthUnit, 
        input[1] * conv->lengthUnit, 
        input[2] * conv->lengthUnit);
    coord::GradCar grad;
    ((PotentialObject*)obj)->pot->eval(point, NULL, &grad);
    // unit of force per unit mass is V/T
    const double convF = 1 / (conv->velocityUnit/conv->timeUnit);
    result[0] = -grad.dx * convF;
    result[1] = -grad.dy * convF;
    result[2] = -grad.dz * convF;
}
static PyObject* Potential_force(PyObject* self, PyObject* args) {
    if(!Potential_isCorrect(self))
        return NULL;
    return callAnyFunctionOnArray<INPUT_VALUE_TRIPLET, OUTPUT_VALUE_TRIPLET>
        (self, args, fncForce);
}

static void fncForceDeriv(PyObject* obj, const double input[], double *result) {
    const coord::PosCar point(
        input[0] * conv->lengthUnit, 
        input[1] * conv->lengthUnit, 
        input[2] * conv->lengthUnit);
    coord::GradCar grad;
    coord::HessCar hess;
    ((PotentialObject*)obj)->pot->eval(point, NULL, &grad, &hess);
    // unit of force per unit mass is V/T
    const double convF = 1 / (conv->velocityUnit/conv->timeUnit);
    // unit of force deriv per unit mass is V/T^2
    const double convD = 1 / (conv->velocityUnit/pow_2(conv->timeUnit));
    result[0] = -grad.dx * convF;
    result[1] = -grad.dy * convF;
    result[2] = -grad.dz * convF;
    result[3] = -hess.dx2  * convD;
    result[4] = -hess.dy2  * convD;
    result[5] = -hess.dz2  * convD;
    result[6] = -hess.dxdy * convD;
    result[7] = -hess.dydz * convD;
    result[8] = -hess.dxdz * convD;
}
static PyObject* Potential_force_deriv(PyObject* self, PyObject* args) {
    if(!Potential_isCorrect(self))
        return NULL;
    return callAnyFunctionOnArray<INPUT_VALUE_TRIPLET, OUTPUT_VALUE_TRIPLET_AND_SEXTET>
        (self, args, fncForceDeriv);
}

static PyObject* Potential_name(PyObject* self)
{
    if(!Potential_isCorrect(self))
        return NULL;
    return Py_BuildValue("s", ((PotentialObject*)self)->pot->name());
}

static PyObject* Potential_export(PyObject* self, PyObject* args)
{
    const char* filename=NULL;
    if(!Potential_isCorrect(self) || !PyArg_ParseTuple(args, "s", &filename))
        return NULL;
    potential::PotentialType t = potential::getPotentialType(*((PotentialObject*)self)->pot);
    if (t != potential::PT_BSE &&
        t != potential::PT_SPLINE &&
        t != potential::PT_CYLSPLINE ) {
        PyErr_SetString(PyExc_ValueError, "Potential is not of an expansion type");
        return NULL;
    }
    try{
        writePotential(filename, *((PotentialObject*)self)->pot);
        Py_INCREF(Py_None);
        return Py_None;
    }
    catch(std::exception& e) {
        PyErr_SetString(PyExc_ValueError, (std::string("Error writing file: ")+e.what()).c_str());
        return NULL;
    }
}

static PyMethodDef Potential_methods[] = {
    { "name", (PyCFunction)Potential_name, METH_NOARGS, 
      "Return the name of the potential\n"
      "No arguments\n"
      "Returns: string" },
    { "potential", Potential_potential, METH_VARARGS, 
      "Compute potential at a given point or array of points\n"
      "Arguments: a triplet of floats (x,y,z) or array of such triplets\n"
      "Returns: float or array of floats" },
    { "density", Potential_density, METH_VARARGS, 
      "Compute density at a given point or array of points\n"
      "Arguments: a triplet of floats (x,y,z) or array of such triplets\n"
      "Returns: float or array of floats" },
    { "force", Potential_force, METH_VARARGS, 
      "Compute force at a given point or array of points\n"
      "Arguments: a triplet of floats (x,y,z) or array of such triplets\n"
      "Returns: float[3] - x,y,z components of force, or array of such triplets" },
    { "force_deriv", Potential_force_deriv, METH_VARARGS, 
      "Compute force and its derivatives at a given point or array of points\n"
      "Arguments: a triplet of floats (x,y,z) or array of such triplets\n"
      "Returns: (float[3],float[6]) - x,y,z components of force, "
      "and the matrix of force derivatives stored as dFx/dx,dFy/dy,dFz/dz,dFx/dy,dFy/dz,dFz/dx; "
      "or if the input was an array of N points, then both items in the tuple are 2d arrays "
      "with sizes Nx3 and Nx6, respectively"},
    { "export", Potential_export, METH_VARARGS, 
      "Export potential expansion coefficients to a text file\n"
      "Arguments: filename (string)\n"
      "Returns: none" },
    { NULL, NULL, 0, NULL }
};

static PyTypeObject PotentialType = {
    PyObject_HEAD_INIT(NULL)
    0, "py_wrapper.Potential",
    sizeof(PotentialObject), 0, (destructor)Potential_dealloc,
    0, 0, 0, 0, 0, 0, 0, 0, 0, Potential_value, Potential_name, 0, 0, 0,
    Py_TPFLAGS_DEFAULT, docstringPotential, 
    0, 0, 0, 0, 0, 0, Potential_methods, 0, 0, 0, 0, 0, 0, 0,
    Potential_init, 0, Potential_new
};

///@}
/// \name  ---------- ActionFinder class and related data ------------
///@{

/// Python type corresponding to ActionFinder class
typedef struct {
    PyObject_HEAD
    const actions::BaseActionFinder* finder;  // C++ class for action finder
    PyObject* pot;  // Python object for potential
} ActionFinderObject;

static PyObject* ActionFinder_new(PyTypeObject *type, PyObject*, PyObject*)
{
    ActionFinderObject *self = (ActionFinderObject*)type->tp_alloc(type, 0);
    if(self) {
        self->finder=NULL;
        self->pot=NULL;
    }
    return (PyObject*)self;
}

static void ActionFinder_dealloc(ActionFinderObject* self)
{
    if(self->finder)
        delete self->finder;
    Py_XDECREF(self->pot);
    self->ob_type->tp_free((PyObject*)self);
}

static const char* docstringActionFinder = "Action finder";

static int ActionFinder_init(PyObject* self, PyObject* args, PyObject* namedargs)
{
    PyObject* objPot=NULL;
    if(!PyArg_ParseTuple(args, "O", &objPot)) {
        PyErr_SetString(PyExc_ValueError, "Incorrect parameters for ActionFinder constructor: "
            "must provide an instance of Potential to work with.");
        return -1;
    }
    if(!PyObject_TypeCheck(objPot, &PotentialType) || 
        ((PotentialObject*)objPot)->pot==NULL ) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a valid instance of Potential class");
        return -1;
    }
    try{
        const actions::BaseActionFinder* finder = 
            new actions::ActionFinderAxisymFudge(*((PotentialObject*)objPot)->pot);
        // ensure valid cleanup if the constructor was called more than once
        if(((ActionFinderObject*)self)->finder!=NULL)
            delete ((ActionFinderObject*)self)->finder;
        Py_XDECREF(((ActionFinderObject*)self)->pot);
        // replace the member variables with freshly created ones
        ((ActionFinderObject*)self)->finder = finder;
        ((ActionFinderObject*)self)->pot = objPot;
        Py_INCREF(objPot);
        return 0;
    }
    catch(std::exception& e) {
        PyErr_SetString(PyExc_ValueError, 
            (std::string("Error in ActionFinder initialization: ")+e.what()).c_str());
        return -1;
    }
}

static void fncActions(PyObject* obj, const double input[], double *result) {
    try{
        const coord::PosVelCar point(
            input[0] * conv->lengthUnit, 
            input[1] * conv->lengthUnit, 
            input[2] * conv->lengthUnit,
            input[3] * conv->velocityUnit, 
            input[4] * conv->velocityUnit, 
            input[5] * conv->velocityUnit);
        actions::Actions acts = ((ActionFinderObject*)obj)->finder->actions(coord::toPosVelCyl(point));
        // unit of action is V*L
        const double convA = 1 / (conv->velocityUnit * conv->lengthUnit);
        result[0] = acts.Jr   * convA;
        result[1] = acts.Jz   * convA;
        result[2] = acts.Jphi * convA;
    }
    catch(std::exception& ) {  // indicates an error, e.g., positive value of energy
        result[0] = result[1] = result[2] = NAN;
    }
}
static PyObject* ActionFinder_actions(PyObject* self, PyObject* args)
{
    if(((ActionFinderObject*)self)->finder==NULL)
        return NULL;
    return callAnyFunctionOnArray<INPUT_VALUE_SEXTET, OUTPUT_VALUE_TRIPLET>
        (self, args, fncActions);
}

static PyMethodDef ActionFinder_methods[] = {
    { "actions", ActionFinder_actions, METH_VARARGS, 
      "Compute actions for a given position/velocity point, or array of points\n"
      "Arguments: a sextet of floats (x,y,z,vx,vy,vz) or array of such sextets\n"
      "Returns: float or array of floats" },
    { NULL, NULL, 0, NULL }
};

static PyTypeObject ActionFinderType = {
    PyObject_HEAD_INIT(NULL)
    0, "py_wrapper.ActionFinder",
    sizeof(ActionFinderObject), 0, (destructor)ActionFinder_dealloc,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    Py_TPFLAGS_DEFAULT, docstringActionFinder, 
    0, 0, 0, 0, 0, 0, ActionFinder_methods, 0, 0, 0, 0, 0, 0, 0,
    ActionFinder_init, 0, ActionFinder_new
};

///@}
/// \name  --------- SplineApprox class -----------
///@{

/// Python type corresponding to SplineApprox class
typedef struct {
    PyObject_HEAD
    math::CubicSpline* spl;
} SplineApproxObject;
       
static PyObject* SplineApprox_new(PyTypeObject *type, PyObject*, PyObject*)
{
    SplineApproxObject *self = (SplineApproxObject*)type->tp_alloc(type, 0);
    if(self)
        self->spl=NULL;
    return (PyObject*)self;
}

static void SplineApprox_dealloc(SplineApproxObject* self)
{
    if(self->spl)
        delete self->spl;
    self->ob_type->tp_free((PyObject*)self);
}

static const char* docstringSplineApprox = 
    "SplineApprox is a class that deals with smoothing splines.\n"
    "It approximates a large set of (x,y) points by a smooth curve with "
    "a rather small number of knots, which should encompass the entire range "
    "of input x values, but preferrably in such a way that each interval "
    "between knots contains at least one x-value from the set of input points.\n"
    "The smoothness of the approximating spline is adjusted by an optional "
    "input parameter `smooth`, which determines the tradeoff between smoothness "
    "and approximation error; zero means no additional smoothing (beyond the one "
    "resulting from discreteness of the spacing of knots), and values around "
    "unity usually yield a reasonable smoothing of noise without sacrificing "
    "too much of accuracy.\n"
    "Values of the spline and up to its second derivative are computed using "
    "the () operator with the first argument being a single x-point or an array "
    "of points, and optional second argument being the derivative index (0, 1, or 2).";

static int SplineApprox_init(PyObject* self, PyObject* args, PyObject* namedargs)
{
    static const char* keywords[] = {"x","y","knots","smooth",NULL};
    PyObject* objx=NULL;
    PyObject* objy=NULL;
    PyObject* objk=NULL;
    double smoothfactor=0;
    if(!PyArg_ParseTupleAndKeywords(args, namedargs, "OOO|d", const_cast<char **>(keywords),
        &objx, &objy, &objk, &smoothfactor)) {
        PyErr_SetString(PyExc_ValueError, "Incorrect parameters passed to the SplineApprox constructor: "
            "must provide two arrays of equal length (input x and y points), "
            "a third array of spline knots, and optionally a float (smooth factor)");
        return -1;
    }
    PyArrayObject *arrx = (PyArrayObject*) PyArray_FROM_OTF(objx, NPY_DOUBLE, NPY_ARRAY_IN_ARRAY);
    PyArrayObject *arry = (PyArrayObject*) PyArray_FROM_OTF(objy, NPY_DOUBLE, NPY_ARRAY_IN_ARRAY);
    PyArrayObject *arrk = (PyArrayObject*) PyArray_FROM_OTF(objk, NPY_DOUBLE, NPY_ARRAY_IN_ARRAY);
    if(arrx == NULL || arry == NULL || arrk == NULL) {
        Py_XDECREF(arrx);
        Py_XDECREF(arry);
        Py_XDECREF(arrk);
        PyErr_SetString(PyExc_ValueError, "Input does not contain valid arrays");
        return -1;
    }
    int numpt = 0;
    if(PyArray_NDIM(arrx) == 1)
        numpt = PyArray_DIM(arrx, 0);
    int numknots = 0;
    if(PyArray_NDIM(arrk) == 1)
        numknots = PyArray_DIM(arrk, 0);
    if(numpt <= 0 || numknots <= 4|| PyArray_NDIM(arry) != 1 || PyArray_DIM(arry, 0) != numpt) {
        Py_DECREF(arrx);
        Py_DECREF(arry);
        Py_DECREF(arrk);
        PyErr_SetString(PyExc_ValueError, 
            "Arguments must be two arrays of equal length (x and y) and a third array (knots, at least 4)");
        return -1;
    }
    std::vector<double> xvalues((double*)PyArray_DATA(arrx), (double*)PyArray_DATA(arrx) + numpt);
    std::vector<double> yvalues((double*)PyArray_DATA(arry), (double*)PyArray_DATA(arry) + numpt);
    std::vector<double> knots((double*)PyArray_DATA(arrk), (double*)PyArray_DATA(arrk) + numknots);
    try{
        math::SplineApprox spl(xvalues, knots);
        std::vector<double> splinevals;
        double der1, der2;
        if(smoothfactor>0)
            spl.fitDataOversmooth(yvalues, smoothfactor, splinevals, der1, der2);
        else
            spl.fitData(yvalues, -smoothfactor, splinevals, der1, der2);
        if(((SplineApproxObject*)self)->spl)  // check if this is not the first time that constructor is called
            delete ((SplineApproxObject*)self)->spl;
        ((SplineApproxObject*)self)->spl = new math::CubicSpline(knots, splinevals, der1, der2);
        return 0;
    }
    catch(std::exception& e) {
        PyErr_SetString(PyExc_ValueError, 
            (std::string("Error in SplineApprox initialization: ")+e.what()).c_str());
        return -1;
    }
}

static double spl_eval(const math::CubicSpline* spl, double x, int der=0)
{
    double result;
    switch(der) {
        case 0: return spl->value(x);
        case 1: spl->evalDeriv(x, NULL, &result); return result;
        case 2: spl->evalDeriv(x, NULL, NULL, &result); return result;
        default: return NAN;
    }
}

static PyObject* SplineApprox_value(PyObject* self, PyObject* args, PyObject* /*kw*/)
{
    PyObject* ptx=NULL;
    int der=0;
    if(self==NULL || ((SplineApproxObject*)self)->spl==NULL || !PyArg_ParseTuple(args, "O|i", &ptx, &der))
        return NULL;
    if(der>2) {
        PyErr_SetString(PyExc_ValueError, "Can only compute derivatives up to 2nd");
        return NULL;
    }
    if(PyFloat_Check(ptx))  // one value
        return Py_BuildValue("d", spl_eval(((SplineApproxObject*)self)->spl, PyFloat_AsDouble(ptx), der) );
    // else an array of values
    PyArrayObject *arr = (PyArrayObject*) 
        PyArray_FROM_OTF(ptx, NPY_DOUBLE, NPY_ARRAY_OUT_ARRAY | NPY_ARRAY_ENSURECOPY);
    if(arr == NULL) {
        PyErr_SetString(PyExc_ValueError, "Argument must be either float, list or numpy array");
        return NULL;
    }
    // replace elements of the copy of input array with computed values
    for(int i=0; i<PyArray_SIZE(arr); i++)
        ((double*)PyArray_DATA(arr))[i] = 
            spl_eval(((SplineApproxObject*)self)->spl, ((double*)PyArray_DATA(arr))[i], der);
    return PyArray_Return(arr);
}

static PyMethodDef SplineApprox_methods[] = {
    { NULL, NULL, 0, NULL }  // no named methods
};

static PyTypeObject SplineApproxType = {
    PyObject_HEAD_INIT(NULL)
    0, "py_wrapper.SplineApprox",
    sizeof(SplineApproxObject), 0, (destructor)SplineApprox_dealloc,
    0, 0, 0, 0, 0, 0, 0, 0, 0, SplineApprox_value, 0, 0, 0, 0,
    Py_TPFLAGS_DEFAULT, docstringSplineApprox, 
    0, 0, 0, 0, 0, 0, SplineApprox_methods, 0, 0, 0, 0, 0, 0, 0,
    SplineApprox_init, 0, SplineApprox_new
};
///@}

static PyMethodDef py_wrapper_methods[] = {
    {"set_units", (PyCFunction)set_units, METH_VARARGS | METH_KEYWORDS, docstringUnits}, 
    {NULL}
};

PyMODINIT_FUNC
initpy_wrapper(void)
{
    PyObject* mod = Py_InitModule("py_wrapper", py_wrapper_methods);
    if(!mod) return;
    conv = new units::ExternalUnits();

    // Potential class
    buildDocstringPotential();
    PotentialTypePtr = &PotentialType;
    if (PyType_Ready(&PotentialType) < 0) return;
    Py_INCREF(&PotentialType);
    PyModule_AddObject(mod, "Potential", (PyObject *)&PotentialType);

    if (PyType_Ready(&ActionFinderType) < 0) return;
    Py_INCREF(&ActionFinderType);
    PyModule_AddObject(mod, "ActionFinder", (PyObject *)&ActionFinderType);
    
    if (PyType_Ready(&SplineApproxType) < 0) return;
    Py_INCREF(&SplineApproxType);
    PyModule_AddObject(mod, "SplineApprox", (PyObject *)&SplineApproxType);

    import_array();
}

/// \endcond