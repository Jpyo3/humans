#!/bin/bash

# List of catkin packages to build
# Order can be important
PACKAGES=("syllo" "videoray" "syllo_rqt" "blueview")

# Generate setenv.sh file on every build
ENV_FILE_NAME="./setenv.sh"
touch ${ENV_FILE_NAME}
chmod +x ${ENV_FILE_NAME}
ENV_FILE="$(readlink -f ${ENV_FILE_NAME})"

THIS_FILE="$(readlink -f ./build.sh)"
echo "#!/bin/bash" > ${ENV_FILE}
echo "# DO NOT EDIT THIS FILE" >> ${ENV_FILE}
echo "# Automatically generated from:" >> ${ENV_FILE}
echo "# ${THIS_FILE}" >> ${ENV_FILE}
echo "" >> ${ENV_FILE}


# Descend into each catkin package and build it
for i in "${PACKAGES[@]}"
do
    :
    pushd "./src/${i}/catkin_ws" >& /dev/null
     # Build the project
    ./build.sh

    # Source the setup.bash script for this project for later projects
    # that will require the current project
    PKG_ENV_FILE="$(readlink -f ./devel/setup.bash)"
    source "${PKG_ENV_FILE}"

    # Add the source line to the high-level setenv.sh script
    echo "source ${PKG_ENV_FILE}" >> ${ENV_FILE}

    popd >& /dev/null
done

echo
echo "=================================================="
echo "  Add the following lines to your .bashrc file    "
echo "=================================================="
echo
echo "CATKIN_WS1_SETUP="${ENV_FILE}
echo 'if [ -f ${CATKIN_WS1_SETUP} ]; then'
echo 'source ${CATKIN_WS1_SETUP}'
echo 'fi'
echo 