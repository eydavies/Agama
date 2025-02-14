'''
Illustration of iterative self-consistent modelling machinery reimplemented in Python.
The two classes Component and SelfConsistentModel are pure Python equivalents of
the same classes provided by the Agama Python interface, and the iterative model results
should be exactly equivalent when using N-body units (unfortunately, when setting
non-trivial dimensional units, the results diverge after first iteration due to
minuscule roundoff effects accumulated in unit conversion).
The concept of a user-defined Python density function that computes moments of
a DF-based GalaxyModel and is used to initialize a cheaper density interpolator
(DensitySphericalHarmonic/DensityAzimuthalHarmonic) can be used in other contexts,
not necessarily within a self-consistent modelling loop.
For instance, one may construct a density expansion for the given DF, and subsequently
use its surfaceDensity() method for many input points -- the result is equivalent
to GalaxyModel.projectedMoments up to integration errors, but is much cheaper to compute.
'''
# if the module has been installed to the globally known directory, just import it
try: import agama
except:  # otherwise load the shared library from the parent folder
    import sys
    sys.path += ['../']
    import agama

class Component:
    '''
    A pure Python analog of agama.Component class (simplified -- no error checking)
    '''
    def __init__(self, **kwargs):
        self.density = None
        self.potential = None
        #self.__dict__.update(kwargs)
        for k,v in kwargs.items(): self.__dict__[k.lower()] = v   # make parameters case-insensitive

    def getDensity(self): return self.density

    def getPotential(self): return self.potential

    def update(self, potential, af):
        # the main procedure for recomputing the density generated by DF
        # and representing it by either a spherical- or azimuthal-harmonic expansion
        if not hasattr(self, 'df'): return   # can only update DF-based components
        gm = agama.GalaxyModel(potential, self.df, af)
        densfnc = lambda x: gm.moments(x, dens=True, vel=False, vel2=False)
        if self.disklike:
            self.density = agama.Density(density=densfnc, type='DensityAzimuthalHarmonic',
                gridsizer=self.sizeradialcyl,   rmin=self.rmincyl, rmax=self.rmaxcyl,
                gridsizez=self.sizeverticalcyl, zmin=self.zmincyl, zmax=self.zmaxcyl,
                mmax=0, symmetry='a')
        else:
            self.density = agama.Density(density=densfnc, type='DensitySphericalHarmonic',
                gridsizer=self.sizeradialsph, rmin=self.rminsph, rmax=self.rmaxsph,
                lmax=self.lmaxangularsph, mmax=0, symmetry='a')

class SelfConsistentModel:
    '''
    A pure Python analog of agama.SelfConsistentModel class (simplified -- no error checking)
    '''
    def __init__(self, **kwargs):
        for k,v in kwargs.items(): self.__dict__[k.lower()] = v
        self.components = []
        self.potential = None
        self.af = None

    def iterate(self):
        if len(self.components)==0:
            raise TypeError("'components' is an empty list")
        # prepare ground: make sure the potential and the corresponding action finder are defined
        if self.potential is None:
            self.updatePotential()
        elif self.af is None:
            self.af = agama.ActionFinder(self.potential)
        # compute the density of all components
        for ic, component in enumerate(self.components):
            print("Computing density for component %i..." % ic)
            component.update(self.potential, self.af)
        # update the total potential and the corresponding action finder
        self.updatePotential()

    def updatePotential(self):
        print("Updating potential...")
        # sort out density and potential components into several groups
        densitySph = []
        densityCyl = []
        potentials = []
        for component in self.components:
            dens = component.getDensity()
            if dens is not None:
                if component.disklike: densityCyl.append(dens)
                else: densitySph.append(dens)
            else:
                potentials.append(component.getPotential())
        # create a single Multipole potential for all non-disk-like density components
        if len(densitySph) > 0:
            potentials.append(agama.Potential(type='Multipole', density=agama.Density(*densitySph),
                gridsizer=self.sizeradialsph, rmin=self.rminsph, rmax=self.rmaxsph,
                lmax=self.lmaxangularsph, mmax=0, symmetry='a'))
        # create a single CylSpline potential representing all disk-like density components
        if len(densityCyl) > 0:
            potentials.append(agama.Potential(type='CylSpline', density=agama.Density(*densityCyl),
                gridsizer=self.sizeradialcyl,   rmin=self.rmincyl, rmax=self.rmaxcyl,
                gridsizez=self.sizeverticalcyl, zmin=self.zmincyl, zmax=self.zmaxcyl,
                mmax=0, symmetry='a'))
        # combine all potential components and reinitialize the action finder
        self.potential = agama.Potential(*potentials)
        print("Updating action finder...")
        self.af = agama.ActionFinder(self.potential)


if __name__ == "__main__":
    # example of usage of both the original SCM classes
    # from the core Agama library and their Python analogues

    def test(Component, SelfConsistentModel):
        # test a two-component spherical model (to speed up things, do not use disky components);
        # the first component is an isotropic DF of a NFW halo with a cutoff,
        # and the second one represents a more concentrated baryonic component,
        # which will cause the adiabatic contraction of the halo
        # when both components are iterated to achieve equilibrium
        params_comp1 = dict(
            disklike = False,
            rminSph = 0.01,
            rmaxSph = 100.0,
            sizeRadialSph = 21,
            lmaxAngularSph = 0)
        params_comp2 = dict(
            disklike = False,
            rminSph = 0.01,
            rmaxSph = 10.0,
            sizeRadialSph = 16,
            lmaxAngularSph = 0)
        params_scm = dict(
            rminSph = 0.005,
            rmaxSph = 200,
            sizeRadialSph = 25,
            lmaxAngularSph = 0)

        potential_init = agama.Potential(type='spheroid',
            gamma=1, beta=3, mass=20.0, scaleRadius=5.0, outerCutoffRadius=40.0)
        df_comp1 = agama.DistributionFunction(type='quasispherical',
            density=potential_init, potential=potential_init)
        df_comp2 = agama.DistributionFunction(type='doublepowerlaw',
            norm=12.0, J0=1.0, coefJrIn=1., coefJzIn=1., coefJrOut=1., coefJzOut=1., slopeIn=1, slopeOut=6)
        model = SelfConsistentModel(**params_scm)
        model.components.append(Component(df=df_comp1, **params_comp1))
        model.components.append(Component(df=df_comp2, **params_comp2))
        model.potential = potential_init
        for it in range(5):
            model.iterate()
        #model.components[0].getDensity().export('comp1')
        #model.components[1].getDensity().export('comp2')
        return agama.GalaxyModel(model.potential, df_comp1).moments([1,0.5,0.3])

    # run the model twice - first with built-in classes from agama, then with their Python analogues
    result1 = test(agama.Component, agama.SelfConsistentModel)
    result2 = test(Component, SelfConsistentModel)

    if result1[0]==result2[0] and all(result1[1]==result2[1]):   # should coincide exactly
        print("\033[1;32mALL TESTS PASSED\033[0m")
    else:
        print("\033[1;31mSOME TESTS FAILED\033[0m")
