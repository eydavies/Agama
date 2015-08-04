/** \file    particles_base.h 
    \brief   Base class for array of particles
    \author  Eugene Vasiliev
    \date    2010-2015
*/
#pragma once
#include "coord.h"
#include <vector>
#include <utility>

/** Classes and functions for manipulating arrays of particles */
namespace particles {

/** Helper class for converting between particle types (from SrcT to DestT).
    This is a templated class with essentially no generic implementation;
    the actual conversion is performed by one of the partially specialized
    template classes below, which are specialized w.r.t. particle type 
    (position/velocity, just position, or something else), but are still generic 
    w.r.t. coordinate conversion.
    Therefore, if one suddenly receives linker error about missing some obscure 
    templated Converter class, it means that one has tried a conversion which is 
    not supported (e.g. converting from position to position/velocity).
*/
template<typename SrcT, typename DestT>
struct Converter {
    DestT operator()(const SrcT& src);
};

/** An array of particles with masses.
    It is implemented as a separate structure instead of just a vector of pairs,
    because of the limitations of C++ that don't allow to template a typedef without a class,
    and to enable seamless conversion between compatible particle types and coordinate systems.
    \tparam ParticleT  is the particle type:
    it could be coord::PosT<coordT> or coord::PosVelT<coordT>, where `coordT`
    is one of the three standard coordinate systems (coord::Car, coord::Cyl, coord::Sph).
    In other words, the particles in this array may have either  positions and masses,
    or positions, velocities and masses.
    The former usage is suitable for potential expansions, as they only need positions;
    seamless conversion ensures that one may supply position/velocity/mass arrays to routines
    that only need position/mass arrays, but not the other way round.
*/
template<typename ParticleT>
struct PointMassArray {
    /// templated typedef of a single particle
    typedef std::pair<ParticleT, double> ElemType;

    /// templated typedef of an array of particles
    typedef std::vector<ElemType> ArrayType;

    /// particles are stored in this array
    ArrayType data;

    ///  default empty constructor
    PointMassArray() {};
    
    /** a seamless conversion constructor from another point mass set 
        with a possibly different template argument.
        \tparam OtherParticleT is a particle type of the source PointMassArray */
    template<typename OtherParticleT> PointMassArray(const PointMassArray<OtherParticleT> &src) {
        data.reserve(src.size());
        Converter<OtherParticleT, ParticleT> conv;  // this is the mighty thing
        for(unsigned int i=0; i<src.size(); i++)
            data.push_back(ElemType(conv(src[i].first), src[i].second));
    }
    
    /// return the array size
    inline unsigned int size() const {
        return data.size(); }

    /// convenience function to add an element
    inline void add(const ParticleT &first, const double second) {
        data.push_back(ElemType(first, second)); }

    /// convenience shorthand for extracting array element
    inline ElemType& at(unsigned int index) {
        return data.at(index); }

    /// convenience shorthand for extracting array element as a const reference
    inline const ElemType& at(unsigned int index) const {
        return data.at(index); }

    /// convenience shorthand for extracting array element
    inline ElemType& operator[](unsigned int index) {
        return data[index]; }

    /// convenience shorthand for extracting array element as a const reference
    inline const ElemType& operator[](unsigned int index) const {
        return data[index]; }

    /// convenience function for extracting the particle (without mass) from the array
    inline const ParticleT& point(unsigned int index) const {
        return data[index].first; }

    /// convenience function for extracting the mass of a particle from the array
    inline double mass(unsigned int index) const {
        return data[index].second; }

    /// return total mass of particles in the array
    inline double totalMass() const {
        double sum=0;
        for(unsigned int i=0; i<data.size(); i++)
            sum += data[i].second;
        return sum;
    }
};

/// more readable typenames for the three coordinate systems
typedef PointMassArray<coord::PosVelCar>  PointMassArrayCar;
typedef PointMassArray<coord::PosVelCyl>  PointMassArrayCyl;
typedef PointMassArray<coord::PosVelSph>  PointMassArraySph;

/// specializations of conversion operator for the case that both SrcT and DestT
/// are pos/vel/mass particle types in possibly different coordinate systems
template<typename SrcCoordT, typename DestCoordT>
struct Converter<coord::PosVelT<SrcCoordT>, coord::PosVelT<DestCoordT> > {
    coord::PosVelT<DestCoordT> operator()(const coord::PosVelT<SrcCoordT>& src) {
        return coord::toPosVel<SrcCoordT, DestCoordT>(src);
    }
};

/// specializations of conversion operator for the case that SrcT is pos/vel/mass 
/// and DestT is pos/mass particle type in possibly different coordinate systems
template<typename SrcCoordT, typename DestCoordT>
struct Converter<coord::PosVelT<SrcCoordT>, coord::PosT<DestCoordT> > {
    coord::PosT<DestCoordT> operator()(const coord::PosVelT<SrcCoordT>& src) {
        return coord::toPos<SrcCoordT, DestCoordT>(src);
    }
};

/// specializations of conversion operator for the case that both SrcT and DestT 
/// are pos/mass particle types in possibly different coordinate systems
template<typename SrcCoordT, typename DestCoordT>
struct Converter<coord::PosT<SrcCoordT>, coord::PosT<DestCoordT> > {
    coord::PosT<DestCoordT> operator()(const coord::PosT<SrcCoordT>& src) {
        return coord::toPos<SrcCoordT, DestCoordT>(src);
    }
};

}  // namespace