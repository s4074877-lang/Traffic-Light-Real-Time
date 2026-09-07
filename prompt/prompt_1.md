# Strucutre
    - central_controller
    - local_intersection_controller
    - train_controller
I qnet have already been setup

Create code for the local_controller to to connected to the central_controller. The loacal will be printing work in the console for local controller example [timestam] RED, [timestam] [timestam] Yellow, [timestam] Green. First start up is will try to connected to the central controller if it false then print ONE time waiting connection formt the central controller and go back prinitng the working, if it get a connection from the central it will print out 1 time the terminal connected to central. DEFFAULT work will be Green and change only after the central send a command
Train_conller will be simular to local-controller but the work is differnt [timestam] Block, [timestam] Open. DEFFAULT work will be Open and only change it the central send a command.

Central have simple UI

=================
CENTRAL CONTROLLER
==================
local_intersection [VM?] [Dsicoonect/Connect] [Work?] [command_sending ?] [timestam_resend_state chane]
train_controller [VM?] [Disconnect/Connect] [Work?] [command_sending ?] [timestam_resend_state chane]
Command: exmaple (train-BLOCK)/(loacl_control-Green)

might improve the ui added some color when needed
central controller will only update the UI if infomation is updated the communation will be using just ONLY QNET for cross communation cation when the command is send it go something centrall -> local -> send_command state update -> central

I only need simple code might have create common folder update the makefile inlucude the common fodler when building I will include that folder with the project folder.

# Name vm
    vm1-local-intersection
    vm2-train-controller
    vm3-central-controller