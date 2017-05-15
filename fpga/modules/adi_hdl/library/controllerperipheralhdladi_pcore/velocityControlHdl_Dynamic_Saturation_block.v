// -------------------------------------------------------------
// 
// File Name: hdl_prj\hdlsrc\controllerPeripheralHdlAdi\velocityControlHdl\velocityControlHdl_Dynamic_Saturation_block.v
// Created: 2014-08-25 21:11:09
// 
// Generated by MATLAB 8.2 and HDL Coder 3.3
// 
// -------------------------------------------------------------


// -------------------------------------------------------------
// 
// Module: velocityControlHdl_Dynamic_Saturation_block
// Source Path: velocityControlHdl/Control_DQ_Currents/Control_Current1/PI_Sat/Dynamic Saturation
// Hierarchy Level: 7
// 
// -------------------------------------------------------------

`timescale 1 ns / 1 ns

module velocityControlHdl_Dynamic_Saturation_block
          (
           up,
           u,
           lo,
           y,
           sat_mode
          );


  input   signed [17:0] up;  // sfix18_En13
  input   signed [35:0] u;  // sfix36_En23
  input   signed [17:0] lo;  // sfix18_En13
  output  signed [35:0] y;  // sfix36_En23
  output  sat_mode;


  wire signed [35:0] LowerRelop1_1_cast;  // sfix36_En23
  wire LowerRelop1_relop1;
  wire signed [35:0] UpperRelop_1_cast;  // sfix36_En23
  wire UpperRelop_relop1;
  wire signed [35:0] lo_dtc;  // sfix36_En23
  wire signed [35:0] Switch_out1;  // sfix36_En23
  wire signed [35:0] up_dtc;  // sfix36_En23
  wire signed [35:0] Switch2_out1;  // sfix36_En23
  wire LowerRelop1_out1;


  // <S24>/LowerRelop1
  assign LowerRelop1_1_cast = {{8{up[17]}}, {up, 10'b0000000000}};
  assign LowerRelop1_relop1 = (u > LowerRelop1_1_cast ? 1'b1 :
              1'b0);



  // <S24>/UpperRelop
  assign UpperRelop_1_cast = {{8{lo[17]}}, {lo, 10'b0000000000}};
  assign UpperRelop_relop1 = (u < UpperRelop_1_cast ? 1'b1 :
              1'b0);



  assign lo_dtc = {{8{lo[17]}}, {lo, 10'b0000000000}};



  // <S24>/Switch
  assign Switch_out1 = (UpperRelop_relop1 == 1'b0 ? u :
              lo_dtc);



  assign up_dtc = {{8{up[17]}}, {up, 10'b0000000000}};



  // <S24>/Switch2
  assign Switch2_out1 = (LowerRelop1_relop1 == 1'b0 ? Switch_out1 :
              up_dtc);



  assign y = Switch2_out1;

  // <S24>/Logical Operator
  assign LowerRelop1_out1 = LowerRelop1_relop1 | UpperRelop_relop1;



  assign sat_mode = LowerRelop1_out1;

endmodule  // velocityControlHdl_Dynamic_Saturation_block

