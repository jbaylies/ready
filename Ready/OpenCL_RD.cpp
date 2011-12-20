/*  Copyright 2011, The Ready Bunch

    This file is part of Ready.

    Ready is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ready is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ready. If not, see <http://www.gnu.org/licenses/>.         */

// local:
#include "OpenCL_RD.hpp"

// STL:
#include <vector>
#include <stdexcept>
#include <utility>
#include <sstream>
#include <cassert>
using namespace std;

// VTK:
#include <vtkImageData.h>

OpenCL_RD::OpenCL_RD()
{
    this->iPlatform = 0;
    this->iDevice = 0;
    this->need_reload_context = true;
    this->kernel_function_name = "rd_compute";
}

OpenCL_RD::~OpenCL_RD()
{
    clReleaseContext(this->context);
    clReleaseCommandQueue(this->command_queue);
    clReleaseKernel(this->kernel);
    clReleaseMemObject(this->buffer1);
    clReleaseMemObject(this->buffer2);
}

void OpenCL_RD::SetPlatform(int i)
{
    if(i != this->iPlatform)
        this->need_reload_context = true;
    this->iPlatform = i;
}

void OpenCL_RD::SetDevice(int i)
{
    if(i != this->iDevice)
        this->need_reload_context = true;
    this->iDevice = i;
}

int OpenCL_RD::GetPlatform() const
{
    return this->iPlatform;
}

int OpenCL_RD::GetDevice() const
{
    return this->iDevice;
}

void OpenCL_RD::throwOnError(cl_int ret,const char* message)
{
    if(ret == CL_SUCCESS) return;

    ostringstream oss;
    oss << message << descriptionOfError(ret);
    throw runtime_error(oss.str().c_str());
}

void OpenCL_RD::ReloadContextIfNeeded()
{
    if(!this->need_reload_context) return;

    cl_int ret;

    // retrieve our chosen platform
    cl_platform_id platform_id;
    {
        const int MAX_PLATFORMS = 10;
        cl_platform_id platforms_available[MAX_PLATFORMS];
        cl_uint num_platforms;
        ret = clGetPlatformIDs(MAX_PLATFORMS,platforms_available,&num_platforms);
        if(ret != CL_SUCCESS || num_platforms==0)
        {
            throw runtime_error("No OpenCL platforms available");
            // currently only likely to see this when running in a virtualized OS, where an opencl.dll is found but doesn't work
        }
        if(this->iPlatform >= (int)num_platforms)
            throw runtime_error("OpenCL_RD::ReloadContextIfNeeded : too few platforms available");
        platform_id = platforms_available[this->iPlatform];
    }

    // retrieve our chosen device
    {
        const int MAX_DEVICES = 10;
        cl_device_id devices_available[MAX_DEVICES];
        cl_uint num_devices;
        ret = clGetDeviceIDs(platform_id,CL_DEVICE_TYPE_ALL,MAX_DEVICES,devices_available,&num_devices);
        throwOnError(ret,"OpenCL_RD::ReloadContextIfNeeded : Failed to retrieve device IDs: ");
        if(this->iDevice >= (int)num_devices)
            throw runtime_error("OpenCL_RD::ReloadContextIfNeeded : too few devices available");
        this->device_id = devices_available[this->iDevice];
    }

    // create the context
    this->context = clCreateContext(NULL,1,&this->device_id,NULL,NULL,&ret);
    throwOnError(ret,"OpenCL_RD::ReloadContextIfNeeded : Failed to create context: ");

    // create the command queue
    this->command_queue = clCreateCommandQueue(this->context,this->device_id,NULL,&ret);
    throwOnError(ret,"OpenCL_RD::ReloadContextIfNeeded : Failed to create command queue: ");

    this->need_reload_context = false;
}

void OpenCL_RD::ReloadKernelIfNeeded()
{
    if(!this->need_reload_program) return;

    cl_int ret;

    // create the program
    const char *source = this->program_string.c_str();
    size_t source_size = this->program_string.length()+1;
    cl_program program = clCreateProgramWithSource(this->context,1,&source,&source_size,&ret);
    throwOnError(ret,"OpenCL_RD::ReloadKernelIfNeeded : Failed to create program with source: ");

    // build the program
    ret = clBuildProgram(program,1,&this->device_id,NULL,NULL,NULL);
    if(ret != CL_SUCCESS)
    {
        const int MAX_BUILD_LOG = 10000;
        char build_log[MAX_BUILD_LOG];
        size_t build_log_length;
        ret = clGetProgramBuildInfo(program,this->device_id,CL_PROGRAM_BUILD_LOG,MAX_BUILD_LOG,build_log,&build_log_length);
        throwOnError(ret,"OpenCL_RD::ReloadKernelIfNeeded : retrieving program build log failed: ");
        ostringstream oss;
        oss << "OpenCL_RD::ReloadKernelIfNeeded : build failed:\n\n" << build_log;
        throw runtime_error(oss.str().c_str());
    }

    // create the kernel
    this->kernel = clCreateKernel(program,this->kernel_function_name.c_str(),&ret);
    throwOnError(ret,"OpenCL_RD::ReloadKernelIfNeeded : kernel creation failed: ");

    // decide the size of the work-groups
    const size_t X = this->GetOldImage()->GetDimensions()[0];
    const size_t Y = this->GetOldImage()->GetDimensions()[1];
    const size_t Z = this->GetOldImage()->GetDimensions()[2];
    this->global_range[0] = X;
    this->global_range[1] = Y;
    this->global_range[2] = Z;
    size_t wgs,returned_size;
    ret = clGetKernelWorkGroupInfo(this->kernel,this->device_id,CL_KERNEL_WORK_GROUP_SIZE,sizeof(size_t),&wgs,&returned_size);
    throwOnError(ret,"OpenCL_RD::ReloadKernelIfNeeded : retrieving kernel work group size failed: ");
    // TODO: allow user override (this value isn't always optimal)
    if(wgs&(wgs-1)) 
        throw runtime_error("OpenCL_RD::ReloadKernelIfNeeded : expecting CL_KERNEL_WORK_GROUP_SIZE to be a power of 2");
    // spread the work group over the dimensions, preferring x over y and y over z because of memory alignment
    size_t wgx,wgy,wgz;
    wgx = min(X,wgs);
    wgy = min(Y,wgs/wgx);
    wgz = min(Z,wgs/(wgx*wgy));
    // TODO: give user control over the work group shape?
    if(X%wgx || Y%wgy || Z%wgz)
        throw runtime_error("OpenCL_RD::ReloadKernelIfNeeded : work group size doesn't divide into grid dimensions");
    this->local_range[0] = wgx;
    this->local_range[1] = wgy;
    this->local_range[2] = wgz;

    this->need_reload_program = false;
}

void OpenCL_RD::CreateOpenCLBuffers()
{
    vtkImageData *old_image = this->GetOldImage();
    vtkImageData *new_image = this->GetNewImage();
    assert(old_image);
    assert(new_image);

    const int X = old_image->GetDimensions()[0];
    const int Y = old_image->GetDimensions()[1];
    const int Z = old_image->GetDimensions()[2];
    const int NC = old_image->GetNumberOfScalarComponents();
    const unsigned long MEM_SIZE = sizeof(float) * X * Y * Z * NC;

    cl_int ret;

    this->buffer1 = clCreateBuffer(this->context, CL_MEM_READ_WRITE, MEM_SIZE, NULL, &ret);
    throwOnError(ret,"OpenCL_RD::CreateBuffers : buffer creation failed: ");

    this->buffer2 = clCreateBuffer(this->context, CL_MEM_READ_WRITE, MEM_SIZE, NULL, &ret);
    throwOnError(ret,"OpenCL_RD::CreateBuffers : buffer creation failed: ");
}

void OpenCL_RD::WriteToOpenCLBuffers()
{
    vtkImageData *old_image = this->GetOldImage();
    vtkImageData *new_image = this->GetNewImage();
    assert(old_image);
    assert(new_image);

    const int X = old_image->GetDimensions()[0];
    const int Y = old_image->GetDimensions()[1];
    const int Z = old_image->GetDimensions()[2];
    const int NC = old_image->GetNumberOfScalarComponents();

    float* old_data = static_cast<float*>(old_image->GetScalarPointer());
    float* new_data = static_cast<float*>(new_image->GetScalarPointer());
    const size_t MEM_SIZE = sizeof(float) * X * Y * Z * NC;

    cl_int ret = clEnqueueWriteBuffer(this->command_queue,this->buffer1, CL_TRUE, 0, MEM_SIZE, old_data, 0, NULL, NULL);
    throwOnError(ret,"OpenCL_RD::WriteToBuffers : buffer writing failed: ");
}

void OpenCL_RD::ReadFromOpenCLBuffers()
{
    vtkImageData *old_image = this->GetOldImage();
    vtkImageData *new_image = this->GetNewImage();
    assert(old_image);
    assert(new_image);

    const int X = old_image->GetDimensions()[0];
    const int Y = old_image->GetDimensions()[1];
    const int Z = old_image->GetDimensions()[2];
    const int NC = old_image->GetNumberOfScalarComponents();

    float* old_data = static_cast<float*>(old_image->GetScalarPointer());
    float* new_data = static_cast<float*>(new_image->GetScalarPointer());
    const size_t MEM_SIZE = sizeof(float) * X * Y * Z * NC;

    cl_int ret = clEnqueueReadBuffer(this->command_queue,this->buffer1, CL_TRUE, 0, MEM_SIZE, new_data, 0, NULL, NULL);
    throwOnError(ret,"OpenCL_RD::ReadFromBuffers : buffer reading failed: ");
}

void OpenCL_RD::Update2Steps()
{
    // (buffer-switching)

    cl_int ret;

    ret = clSetKernelArg(this->kernel, 0, sizeof(cl_mem), (void *)&this->buffer1); // input
    throwOnError(ret,"OpenCL_RD::Update2Steps : clSetKernelArg failed: ");
    ret = clSetKernelArg(this->kernel, 1, sizeof(cl_mem), (void *)&this->buffer2); // output
    throwOnError(ret,"OpenCL_RD::Update2Steps : clSetKernelArg failed: ");
    ret = clEnqueueNDRangeKernel(this->command_queue,this->kernel, 3, NULL, this->global_range, this->local_range, 0, NULL, NULL);
    throwOnError(ret,"OpenCL_RD::Update2Steps : clEnqueueNDRangeKernel failed: ");

    ret = clSetKernelArg(this->kernel, 0, sizeof(cl_mem), (void *)&this->buffer2); // input
    throwOnError(ret,"OpenCL_RD::Update2Steps : clSetKernelArg failed: ");
    ret = clSetKernelArg(this->kernel, 1, sizeof(cl_mem), (void *)&this->buffer1); // output
    throwOnError(ret,"OpenCL_RD::Update2Steps : clSetKernelArg failed: ");
    ret = clEnqueueNDRangeKernel(this->command_queue,this->kernel, 3, NULL, this->global_range, this->local_range, 0, NULL, NULL);
    throwOnError(ret,"OpenCL_RD::Update2Steps : clEnqueueNDRangeKernel failed: ");
}

void OpenCL_RD::TestProgram(std::string test_program_string) const
{
    cl_int ret;

    // create the program
    const char *source = test_program_string.c_str();
    size_t source_size = test_program_string.length()+1;
    cl_program program = clCreateProgramWithSource(this->context,1,&source,&source_size,&ret);
    throwOnError(ret,"OpenCL_RD::TestProgram : Failed to create program with source: ");

    // build the program
    ret = clBuildProgram(program,1,&this->device_id,NULL,NULL,NULL);
    if(ret != CL_SUCCESS)
    {
        const int MAX_BUILD_LOG = 10000;
        char build_log[MAX_BUILD_LOG];
        size_t build_log_length;
        ret = clGetProgramBuildInfo(program,this->device_id,CL_PROGRAM_BUILD_LOG,MAX_BUILD_LOG,build_log,&build_log_length);
        throwOnError(ret,"OpenCL_RD::TestProgram : retrieving program build log failed: ");
        ostringstream oss;
        oss << "OpenCL_RD::TestProgram : build failed:\n\n" << build_log;
        throw runtime_error(oss.str().c_str());
    }
}

std::string OpenCL_RD::GetOpenCLDiagnostics() // report on OpenCL without throwing exceptions
{
    // TODO: make this report more readable, retrieve numeric data too
    ostringstream report;

    // get available OpenCL platforms
    const size_t MAX_PLATFORMS = 10;
    cl_platform_id platforms_available[MAX_PLATFORMS];
    cl_uint num_platforms;
    cl_int ret = clGetPlatformIDs(MAX_PLATFORMS,platforms_available,&num_platforms);
    if(ret != CL_SUCCESS || num_platforms==0)
    {
        report << "No OpenCL platforms available";
        // currently only likely to see this when running in a virtualized OS, where an opencl.dll is found but doesn't work
        return report.str();
    }

    report << "Found " << num_platforms << " platform(s):\n";

    for(unsigned int iPlatform=0;iPlatform<num_platforms;iPlatform++)
    {
        report << "Platform " << iPlatform+1 << ":\n";
        const size_t MAX_INFO_LENGTH = 1000;
        char info[MAX_INFO_LENGTH];
        size_t info_length;
        for(cl_platform_info i=CL_PLATFORM_PROFILE;i<=CL_PLATFORM_EXTENSIONS;i++)
        {
            clGetPlatformInfo(platforms_available[iPlatform],i,MAX_INFO_LENGTH,info,&info_length);
            report << i << " : " << info << "\n";
        }

        const size_t MAX_DEVICES = 10;
        cl_device_id devices_available[MAX_DEVICES];
        cl_uint num_devices;
        clGetDeviceIDs(platforms_available[iPlatform],CL_DEVICE_TYPE_ALL,MAX_DEVICES,devices_available,&num_devices);
        report << "\nFound " << num_devices << " device(s) on this platform.\n";
        for(unsigned int iDevice=0;iDevice<num_devices;iDevice++)
        {
            report << "Device " << iDevice+1 << ":\n";
            for(cl_device_info i=CL_DEVICE_NAME;i<=CL_DEVICE_EXTENSIONS;i++)
            {
                clGetDeviceInfo(devices_available[iDevice],i,MAX_INFO_LENGTH,info,&info_length);
                report << i << " : " << info << "\n";
            }
        }
        report << "\n";
    }

    return report.str();
}

// http://www.khronos.org/message_boards/viewtopic.php?f=37&t=2107
const char* OpenCL_RD::descriptionOfError(cl_int err) 
{
    switch (err) {
        case CL_SUCCESS:                            return "Success!";
        case CL_DEVICE_NOT_FOUND:                   return "Device not found.";
        case CL_DEVICE_NOT_AVAILABLE:               return "Device not available";
        case CL_COMPILER_NOT_AVAILABLE:             return "Compiler not available";
        case CL_MEM_OBJECT_ALLOCATION_FAILURE:      return "Memory object allocation failure";
        case CL_OUT_OF_RESOURCES:                   return "Out of resources";
        case CL_OUT_OF_HOST_MEMORY:                 return "Out of host memory";
        case CL_PROFILING_INFO_NOT_AVAILABLE:       return "Profiling information not available";
        case CL_MEM_COPY_OVERLAP:                   return "Memory copy overlap";
        case CL_IMAGE_FORMAT_MISMATCH:              return "Image format mismatch";
        case CL_IMAGE_FORMAT_NOT_SUPPORTED:         return "Image format not supported";
        case CL_BUILD_PROGRAM_FAILURE:              return "Program build failure";
        case CL_MAP_FAILURE:                        return "Map failure";
        case CL_INVALID_VALUE:                      return "Invalid value";
        case CL_INVALID_DEVICE_TYPE:                return "Invalid device type";
        case CL_INVALID_PLATFORM:                   return "Invalid platform";
        case CL_INVALID_DEVICE:                     return "Invalid device";
        case CL_INVALID_CONTEXT:                    return "Invalid context";
        case CL_INVALID_QUEUE_PROPERTIES:           return "Invalid queue properties";
        case CL_INVALID_COMMAND_QUEUE:              return "Invalid command queue";
        case CL_INVALID_HOST_PTR:                   return "Invalid host pointer";
        case CL_INVALID_MEM_OBJECT:                 return "Invalid memory object";
        case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR:    return "Invalid image format descriptor";
        case CL_INVALID_IMAGE_SIZE:                 return "Invalid image size";
        case CL_INVALID_SAMPLER:                    return "Invalid sampler";
        case CL_INVALID_BINARY:                     return "Invalid binary";
        case CL_INVALID_BUILD_OPTIONS:              return "Invalid build options";
        case CL_INVALID_PROGRAM:                    return "Invalid program";
        case CL_INVALID_PROGRAM_EXECUTABLE:         return "Invalid program executable";
        case CL_INVALID_KERNEL_NAME:                return "Invalid kernel name";
        case CL_INVALID_KERNEL_DEFINITION:          return "Invalid kernel definition";
        case CL_INVALID_KERNEL:                     return "Invalid kernel";
        case CL_INVALID_ARG_INDEX:                  return "Invalid argument index";
        case CL_INVALID_ARG_VALUE:                  return "Invalid argument value";
        case CL_INVALID_ARG_SIZE:                   return "Invalid argument size";
        case CL_INVALID_KERNEL_ARGS:                return "Invalid kernel arguments";
        case CL_INVALID_WORK_DIMENSION:             return "Invalid work dimension";
        case CL_INVALID_WORK_GROUP_SIZE:            return "Invalid work group size";
        case CL_INVALID_WORK_ITEM_SIZE:             return "Invalid work item size";
        case CL_INVALID_GLOBAL_OFFSET:              return "Invalid global offset";
        case CL_INVALID_EVENT_WAIT_LIST:            return "Invalid event wait list";
        case CL_INVALID_EVENT:                      return "Invalid event";
        case CL_INVALID_OPERATION:                  return "Invalid operation";
        case CL_INVALID_GL_OBJECT:                  return "Invalid OpenGL object";
        case CL_INVALID_BUFFER_SIZE:                return "Invalid buffer size";
        case CL_INVALID_MIP_LEVEL:                  return "Invalid mip-map level";
        default: return "Unknown";
    }
}
