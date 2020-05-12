# should be moved into metrics folder which is inside of canga folder
CANGA_FOLDER=/ascldap/users/pakuber/Compadre/compadre/test_data/grids/canga
DATA_FOLDER=/scratch/pakuber
EXE_FOLDER=/ascldap/users/pakuber/Compadre/compadre/build/examples

for i in {0..0}; do python $CANGA_FOLDER/remap_single_call.py --exe-folder-absolute=$EXE_FOLDER --canga-folder-absolute=$CANGA_FOLDER/NM16 --output-folder-absolute=$DATA_FOLDER --total-iterations=30 --optimization="CAAS" --mesh-1-type=CS --mesh-1=$i --mesh-2-type=CVT --mesh-2=$i --start-step=1 --porder=2 --save-every=10; done;
