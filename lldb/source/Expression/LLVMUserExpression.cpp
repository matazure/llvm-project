//===-- LLVMUserExpression.cpp ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
// C++ Includes

// Project includes
#include "lldb/Expression/LLVMUserExpression.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/ValueObjectConstResult.h"
#include "lldb/Expression/ExpressionSourceCode.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Expression/IRInterpreter.h"
#include "lldb/Expression/Materializer.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/ClangExternalASTSourceCommon.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadPlan.h"
#include "lldb/Target/ThreadPlanCallUserExpression.h"

using namespace lldb_private;

LLVMUserExpression::LLVMUserExpression(ExecutionContextScope &exe_scope, 
                                       const char *expr, 
                                       const char *expr_prefix,
                                       lldb::LanguageType language, 
                                       ResultType desired_type, 
                                       const EvaluateExpressionOptions &options)
    : UserExpression(exe_scope, expr, expr_prefix, language, desired_type, options),
      m_stack_frame_bottom(LLDB_INVALID_ADDRESS),
      m_stack_frame_top(LLDB_INVALID_ADDRESS),
      m_transformed_text(),
      m_execution_unit_sp(),
      m_materializer_ap(),
      m_jit_module_wp(),
      m_enforce_valid_object(true),
      m_in_cplusplus_method(false),
      m_in_objectivec_method(false),
      m_in_static_method(false),
      m_needs_object_ptr(false),
      m_const_object(false),
      m_target(NULL),
      m_can_interpret(false),
      m_materialized_address(LLDB_INVALID_ADDRESS)
{
}

LLVMUserExpression::~LLVMUserExpression()
{
    if (m_target)
    {
        lldb::ModuleSP jit_module_sp(m_jit_module_wp.lock());
        if (jit_module_sp)
            m_target->GetImages().Remove(jit_module_sp);
    }
}

lldb::ExpressionResults
LLVMUserExpression::Execute(Stream &error_stream, ExecutionContext &exe_ctx, const EvaluateExpressionOptions &options,
                            lldb::UserExpressionSP &shared_ptr_to_me, lldb::ExpressionVariableSP &result)
{
    // The expression log is quite verbose, and if you're just tracking the execution of the
    // expression, it's quite convenient to have these logs come out with the STEP log as well.
    Log *log(lldb_private::GetLogIfAnyCategoriesSet(LIBLLDB_LOG_EXPRESSIONS | LIBLLDB_LOG_STEP));

    if (m_jit_start_addr != LLDB_INVALID_ADDRESS || m_can_interpret)
    {
        lldb::addr_t struct_address = LLDB_INVALID_ADDRESS;

        if (!PrepareToExecuteJITExpression(error_stream, exe_ctx, struct_address))
        {
            error_stream.Printf("Errored out in %s, couldn't PrepareToExecuteJITExpression", __FUNCTION__);
            return lldb::eExpressionSetupError;
        }

        lldb::addr_t function_stack_bottom = LLDB_INVALID_ADDRESS;
        lldb::addr_t function_stack_top = LLDB_INVALID_ADDRESS;

        if (m_can_interpret)
        {
            llvm::Module *module = m_execution_unit_sp->GetModule();
            llvm::Function *function = m_execution_unit_sp->GetFunction();

            if (!module || !function)
            {
                error_stream.Printf("Supposed to interpret, but nothing is there");
                return lldb::eExpressionSetupError;
            }

            Error interpreter_error;

            std::vector<lldb::addr_t> args;

            if (!AddInitialArguments(exe_ctx, args, error_stream))
            {
                error_stream.Printf("Errored out in %s, couldn't AddInitialArguments", __FUNCTION__);
                return lldb::eExpressionSetupError;
            }

            args.push_back(struct_address);

            function_stack_bottom = m_stack_frame_bottom;
            function_stack_top = m_stack_frame_top;

            IRInterpreter::Interpret(*module, *function, args, *m_execution_unit_sp.get(), interpreter_error,
                                     function_stack_bottom, function_stack_top, exe_ctx);

            if (!interpreter_error.Success())
            {
                error_stream.Printf("Supposed to interpret, but failed: %s", interpreter_error.AsCString());
                return lldb::eExpressionDiscarded;
            }
        }
        else
        {
            if (!exe_ctx.HasThreadScope())
            {
                error_stream.Printf("UserExpression::Execute called with no thread selected.");
                return lldb::eExpressionSetupError;
            }

            Address wrapper_address(m_jit_start_addr);

            std::vector<lldb::addr_t> args;

            if (!AddInitialArguments(exe_ctx, args, error_stream))
            {
                error_stream.Printf("Errored out in %s, couldn't AddInitialArguments", __FUNCTION__);
                return lldb::eExpressionSetupError;
            }

            args.push_back(struct_address);

            lldb::ThreadPlanSP call_plan_sp(new ThreadPlanCallUserExpression(exe_ctx.GetThreadRef(), wrapper_address,
                                                                             args, options, shared_ptr_to_me));

            if (!call_plan_sp || !call_plan_sp->ValidatePlan(&error_stream))
                return lldb::eExpressionSetupError;

            ThreadPlanCallUserExpression *user_expression_plan =
                static_cast<ThreadPlanCallUserExpression *>(call_plan_sp.get());

            lldb::addr_t function_stack_pointer = user_expression_plan->GetFunctionStackPointer();

            function_stack_bottom = function_stack_pointer - HostInfo::GetPageSize();
            function_stack_top = function_stack_pointer;

            if (log)
                log->Printf("-- [UserExpression::Execute] Execution of expression begins --");

            if (exe_ctx.GetProcessPtr())
                exe_ctx.GetProcessPtr()->SetRunningUserExpression(true);

            lldb::ExpressionResults execution_result =
                exe_ctx.GetProcessRef().RunThreadPlan(exe_ctx, call_plan_sp, options, error_stream);

            if (exe_ctx.GetProcessPtr())
                exe_ctx.GetProcessPtr()->SetRunningUserExpression(false);

            if (log)
                log->Printf("-- [UserExpression::Execute] Execution of expression completed --");

            if (execution_result == lldb::eExpressionInterrupted || execution_result == lldb::eExpressionHitBreakpoint)
            {
                const char *error_desc = NULL;

                if (call_plan_sp)
                {
                    lldb::StopInfoSP real_stop_info_sp = call_plan_sp->GetRealStopInfo();
                    if (real_stop_info_sp)
                        error_desc = real_stop_info_sp->GetDescription();
                }
                if (error_desc)
                    error_stream.Printf("Execution was interrupted, reason: %s.", error_desc);
                else
                    error_stream.PutCString("Execution was interrupted.");

                if ((execution_result == lldb::eExpressionInterrupted && options.DoesUnwindOnError()) ||
                    (execution_result == lldb::eExpressionHitBreakpoint && options.DoesIgnoreBreakpoints()))
                    error_stream.PutCString(
                        "\nThe process has been returned to the state before expression evaluation.");
                else
                {
                    if (execution_result == lldb::eExpressionHitBreakpoint)
                        user_expression_plan->TransferExpressionOwnership();
                    error_stream.PutCString(
                        "\nThe process has been left at the point where it was interrupted, "
                        "use \"thread return -x\" to return to the state before expression evaluation.");
                }

                return execution_result;
            }
            else if (execution_result == lldb::eExpressionStoppedForDebug)
            {
                error_stream.PutCString(
                    "Execution was halted at the first instruction of the expression "
                    "function because \"debug\" was requested.\n"
                    "Use \"thread return -x\" to return to the state before expression evaluation.");
                return execution_result;
            }
            else if (execution_result != lldb::eExpressionCompleted)
            {
                error_stream.Printf("Couldn't execute function; result was %s\n",
                                    Process::ExecutionResultAsCString(execution_result));
                return execution_result;
            }
        }

        if (FinalizeJITExecution(error_stream, exe_ctx, result, function_stack_bottom, function_stack_top))
        {
            return lldb::eExpressionCompleted;
        }
        else
        {
            return lldb::eExpressionResultUnavailable;
        }
    }
    else
    {
        error_stream.Printf("Expression can't be run, because there is no JIT compiled function");
        return lldb::eExpressionSetupError;
    }
}

bool
LLVMUserExpression::FinalizeJITExecution(Stream &error_stream, ExecutionContext &exe_ctx,
                                         lldb::ExpressionVariableSP &result, lldb::addr_t function_stack_bottom,
                                         lldb::addr_t function_stack_top)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

    if (log)
        log->Printf("-- [UserExpression::FinalizeJITExecution] Dematerializing after execution --");

    if (!m_dematerializer_sp)
    {
        error_stream.Printf("Couldn't apply expression side effects : no dematerializer is present");
        return false;
    }

    Error dematerialize_error;

    m_dematerializer_sp->Dematerialize(dematerialize_error, function_stack_bottom, function_stack_top);

    if (!dematerialize_error.Success())
    {
        error_stream.Printf("Couldn't apply expression side effects : %s\n",
                            dematerialize_error.AsCString("unknown error"));
        return false;
    }

    result = GetResultAfterDematerialization(exe_ctx.GetBestExecutionContextScope());

    if (result)
        result->TransferAddress();

    m_dematerializer_sp.reset();

    return true;
}

bool
LLVMUserExpression::PrepareToExecuteJITExpression(Stream &error_stream, ExecutionContext &exe_ctx,
                                                  lldb::addr_t &struct_address)
{
    lldb::TargetSP target;
    lldb::ProcessSP process;
    lldb::StackFrameSP frame;

    if (!LockAndCheckContext(exe_ctx, target, process, frame))
    {
        error_stream.Printf("The context has changed before we could JIT the expression!\n");
        return false;
    }

    if (m_jit_start_addr != LLDB_INVALID_ADDRESS || m_can_interpret)
    {
        if (m_materialized_address == LLDB_INVALID_ADDRESS)
        {
            Error alloc_error;

            IRMemoryMap::AllocationPolicy policy =
                m_can_interpret ? IRMemoryMap::eAllocationPolicyHostOnly : IRMemoryMap::eAllocationPolicyMirror;

            m_materialized_address = m_execution_unit_sp->Malloc(
                m_materializer_ap->GetStructByteSize(), m_materializer_ap->GetStructAlignment(),
                lldb::ePermissionsReadable | lldb::ePermissionsWritable, policy, alloc_error);

            if (!alloc_error.Success())
            {
                error_stream.Printf("Couldn't allocate space for materialized struct: %s\n", alloc_error.AsCString());
                return false;
            }
        }

        struct_address = m_materialized_address;

        if (m_can_interpret && m_stack_frame_bottom == LLDB_INVALID_ADDRESS)
        {
            Error alloc_error;

            const size_t stack_frame_size = 512 * 1024;

            m_stack_frame_bottom = m_execution_unit_sp->Malloc(stack_frame_size, 8,
                                                               lldb::ePermissionsReadable | lldb::ePermissionsWritable,
                                                               IRMemoryMap::eAllocationPolicyHostOnly, alloc_error);

            m_stack_frame_top = m_stack_frame_bottom + stack_frame_size;

            if (!alloc_error.Success())
            {
                error_stream.Printf("Couldn't allocate space for the stack frame: %s\n", alloc_error.AsCString());
                return false;
            }
        }

        Error materialize_error;

        m_dematerializer_sp =
            m_materializer_ap->Materialize(frame, *m_execution_unit_sp, struct_address, materialize_error);

        if (!materialize_error.Success())
        {
            error_stream.Printf("Couldn't materialize: %s\n", materialize_error.AsCString());
            return false;
        }
    }
    return true;
}

lldb::ModuleSP
LLVMUserExpression::GetJITModule()
{
    if (m_execution_unit_sp)
        return m_execution_unit_sp->GetJITModule();
    return lldb::ModuleSP();
}
