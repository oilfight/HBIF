{
    word alphaScheme("div(phi,alpha)");
    word alpharScheme("div(phirb,alpha)");

    tmp<fv::ddtScheme<scalar>> ddtAlpha
    (
        fv::ddtScheme<scalar>::New
        (
            mesh,
            mesh.ddtScheme("ddt(alpha)")
        )
    );

    // Set the off-centering coefficient according to ddt scheme
    scalar ocCoeff = 0;
    if
    (
        isType<fv::EulerDdtScheme<scalar>>(ddtAlpha())
     || isType<fv::localEulerDdtScheme<scalar>>(ddtAlpha())
    )
    {
        ocCoeff = 0;
    }
    else if (isType<fv::CrankNicolsonDdtScheme<scalar>>(ddtAlpha()))
    {
        if (nAlphaSubCycles > 1)
        {
            FatalErrorInFunction
                << "Sub-cycling is not supported "
                   "with the CrankNicolson ddt scheme"
                << exit(FatalError);
        }

        ocCoeff =
            refCast<const fv::CrankNicolsonDdtScheme<scalar>>(ddtAlpha())
           .ocCoeff();
    }
    else
    {
        FatalErrorInFunction
            << "Only Euler and CrankNicolson ddt schemes are supported"
            << exit(FatalError);
    }

    scalar cnCoeff = 1.0/(1.0 + ocCoeff);

    // Standard face-flux compression coefficient
    surfaceScalarField phic(mixture.cAlpha()*mag(phi/mesh.magSf())); 

    // Add the optional isotropic compression contribution
    if (icAlpha > 0)
    {
        phic *= (1.0 - icAlpha);
        phic += (mixture.cAlpha()*icAlpha)*fvc::interpolate(mag(U));
    }

    // Add the optional shear compression contribution
    if (scAlpha > 0)
    {
        phic +=
            scAlpha*mag(mesh.delta() & fvc::interpolate(symm(fvc::grad(U))));
    }

    //Calculating Porous Media Velocities

    Uwetting = Solid*M1*(
	 - fvc::grad(p) 
	 + (1.-alpha1)*fvc::grad(pc) 
         -  pc*fvc::grad(alpha1) 
         +  rho1*g
	 );

    UnonWetting = Solid*M2*(
	 -fvc::grad(p) 
	 -alpha1*fvc::grad(pc) 
         -pc*fvc::grad(alpha1)
         +rho2*g
	);

    Uwetting.correctBoundaryConditions();  
    UnonWetting.correctBoundaryConditions();

    // Defining relative velocity
    Ur = ( Uwetting/max(alpha1,0.01) - UnonWetting/max((1.-alpha1),0.01) )/max(eps,0.01);
    
    //Test border attenuation 
    volVectorField SolidBorder = fvc::grad(mag(fvc::grad(Solid))); 
    //volVectorField SolidBorder = fvc::grad(Solid);
    forAll (Ur,celli)
    {
    	if (mag(SolidBorder[celli])>0)
    	{Ur[celli]= vector(0,0,0);}

        //if (mag(Ur[celli]) > 1e2) 
	//{Ur[celli] = vector(0,0,0);}
    }
   
    surfaceScalarField phirPore(fvc::interpolate(Ur) & mesh.Sf());

    surfaceScalarField::Boundary& phicBf =
    	phic.boundaryFieldRef();
    surfaceScalarField::Boundary& phirPoreBf =
    	phirPore.boundaryFieldRef(); 
   
    // Do not compress interface at non-coupled boundary faces (inlets, outlets etc.)
    forAll(phic.boundaryField(), patchi)
    {
        fvsPatchScalarField& phicp = phicBf[patchi];

        if (!phicp.coupled())
        {
            phicp == 0;
        }
    }

    // Correcting Ur at boundaries
    forAll(phirPore.boundaryField(), patchi) 
    {

        fvsPatchScalarField& phirPorep = phirPoreBf[patchi];

        if (!phirPorep.coupled())
        { 
	      phirPorep == 0; 
        }

        if ( (isA< fixedValueFvPatchField<vector> >(Uwetting.boundaryField()[patchi])) or 
	     (isA< fixedValueFvPatchField<vector> >(UnonWetting.boundaryField()[patchi])) )
        {
	      // Ur = U1/phiS1 -(U2)/phiS2 at the boundary
              phirPoreBf[patchi] =
            		  (Uwetting.boundaryField()[patchi] & mesh.Sf().boundaryField()[patchi])
             		 /(eps.boundaryField()[patchi]*alpha1.boundaryField()[patchi] + 0.001)
                     	 -(UnonWetting.boundaryField()[patchi] & mesh.Sf().boundaryField()[patchi])
                     	 /(eps.boundaryField()[patchi]*(1-alpha1.boundaryField()[patchi]) + 0.001);
	}
    }

    #include "correctUBc.H"

    tmp<surfaceScalarField> phiCN(phi);

    // Calculate the Crank-Nicolson off-centred volumetric flux
    if (ocCoeff > 0)
    {
        phiCN = cnCoeff*phi + (1.0 - cnCoeff)*phi.oldTime();
    }

    if (MULESCorr)
    {
        fvScalarMatrix alpha1Eqn
        (
            (
                LTS
              ? fv::localEulerDdtScheme<scalar>(mesh).fvmDdt(eps,alpha1)
              : fv::EulerDdtScheme<scalar>(mesh).fvmDdt(eps,alpha1)
            )
          + fv::gaussConvectionScheme<scalar>
            (
                mesh,
                phiCN,
                upwind<scalar>(mesh, phiCN)
            ).fvmDiv(phiCN, alpha1)
        );

        alpha1Eqn.solve();

        Info<< "Phase-1 volume fraction = "
            << alpha1.weightedAverage(mesh.Vsc()).value()
            << "  Min(" << alpha1.name() << ") = " << min(alpha1).value()
            << "  Max(" << alpha1.name() << ") = " << max(alpha1).value()
            << endl;

        tmp<surfaceScalarField> talphaPhiUD(alpha1Eqn.flux());
        alphaPhi = talphaPhiUD();

        if (alphaApplyPrevCorr && talphaPhiCorr0.valid())
        {
            Info<< "Applying the previous iteration compression flux" << endl; 
            
	    MULES::correct
            (
                eps,
                alpha1,
                alphaPhi,
                talphaPhiCorr0.ref(),
		zeroField(),
		zeroField(),
                oneField(),
                zeroField()
            );

            alphaPhi += talphaPhiCorr0();
        }

        // Cache the upwind-flux
        talphaPhiCorr0 = talphaPhiUD;

        alpha2 = 1.0 - alpha1;

        mixture.correct();
    }


    for (int aCorr=0; aCorr<nAlphaCorr; aCorr++)
    {  
           
        surfaceScalarField phir(
                           (1.-Solidf)*phic*mixture.nHatf()
                           + Solidf*phirPore
                           );
        
        tmp<surfaceScalarField> talphaPhiUn
        (
            fvc::flux
            (
                phi,
                alpha1,
                alphaScheme
            )
          + fvc::flux
            (
                -fvc::flux(-phir, (alpha2*eps), alpharScheme), 
		alpha1,
                alpharScheme
            )
        );

        // Calculate the Crank-Nicolson off-centred alpha flux
        if (ocCoeff > 0)
        {
            talphaPhiUn =
                cnCoeff*talphaPhiUn + (1.0 - cnCoeff)*alphaPhi.oldTime();
        }

        if (MULESCorr)
        {
            tmp<surfaceScalarField> talphaPhiCorr(talphaPhiUn() - alphaPhi);
            volScalarField alpha10("alpha10", alpha1);
 
            MULES::correct
            (
                eps,
                alpha1,
                talphaPhiUn(),
                talphaPhiCorr.ref(),
		zeroField(),
		zeroField(),
                oneField(),
                zeroField()
            );

            // Under-relax the correction for all but the 1st corrector
            if (aCorr == 0)
            {
                alphaPhi += talphaPhiCorr();
            }
            else
            {
                alpha1 = 0.5*alpha1 + 0.5*alpha10;
                alphaPhi += 0.5*talphaPhiCorr();
            }
        }
        else
        {
            alphaPhi = talphaPhiUn; 
            MULES::explicitSolve
            (
                eps,
                alpha1,
                phiCN,
                alphaPhi,
		zeroField(),
		zeroField(),
	        oneField(),
                zeroField()
            );
        }

        alpha2 = 1.0 - alpha1;

        mixture.correct();
    }

    if (alphaApplyPrevCorr && MULESCorr)
    {
        talphaPhiCorr0 = alphaPhi - talphaPhiCorr0;
    }

    if
    (
        word(mesh.ddtScheme("ddt(rho,U)"))
     == fv::EulerDdtScheme<vector>::typeName
    )
    {
        rhoPhi = alphaPhi*(rho1 - rho2) + phiCN*rho2;
    }
    else
    {
        if (ocCoeff > 0)
        {
            // Calculate the end-of-time-step alpha flux
            alphaPhi = (alphaPhi - (1.0 - cnCoeff)*alphaPhi.oldTime())/cnCoeff;
        }

            // Calculate the end-of-time-step mass flux
        rhoPhi = alphaPhi*(rho1 - rho2) + phi*rho2;
    }

    Info<< "Phase-1 volume fraction = "
        << alpha1.weightedAverage(mesh.Vsc()).value()
        << "  Min(" << alpha1.name() << ") = " << min(alpha1).value()
        << "  Max(" << alpha1.name() << ") = " << max(alpha1).value()
        << endl;
}
