#if defined(TAP_DANCE_ENABLE)

  #if defined(BETTER_DRAGSCROLL_TAPDANCE)
    #include "better_dragscroll.h"
    typedef struct {
      bool is_press_action;
      int state;
    } tap;
    enum {
      SINGLE_TAP = 1,
      SINGLE_HOLD = 2,
      DOUBLE_TAP = 3,
      DOUBLE_HOLD = 4,
      DOUBLE_SINGLE_TAP = 5, //send two single taps
      TRIPLE_TAP = 6,
      TRIPLE_HOLD = 7
    };
    //instanalize an instance of 'tap' for the 'x' tap dance.
    static tap tap_state = {
      .is_press_action = true,
      .state = 0
    };
    int cur_dance (tap_dance_state_t *state) {
      if (state->count == 1) {
        if (state->interrupted || !state->pressed)  return SINGLE_TAP;
        //key has not been interrupted, but they key is still held. Means you want to send a 'HOLD'.
        else return SINGLE_HOLD;
      }
      else if (state->count == 2) {
        /*
        * DOUBLE_SINGLE_TAP is to distinguish between typing "pepper", and actually wanting a double tap
        * action when hitting 'pp'. Suggested use case for this return value is when you want to send two
        * keystrokes of the key, and not the 'double tap' action/macro.
        */
        if (state->interrupted) return DOUBLE_SINGLE_TAP;
        else if (state->pressed) return DOUBLE_HOLD;
        else return DOUBLE_TAP;
      }
      //Assumes no one is trying to type the same letter three times (at least not quickly).
      //If your tap dance key is 'KC_W', and you want to type "www." quickly - then you will need to add
      //an exception here to return a 'TRIPLE_SINGLE_TAP', and define that enum just like 'DOUBLE_SINGLE_TAP'
      if (state->count == 3) {
        if (state->interrupted || !state->pressed)  return TRIPLE_TAP;
        else return TRIPLE_HOLD;
      }
      else return 8; //magic number. At some point this method will expand to work for more presses
    }

    void dragscrolltd_fin(tap_dance_state_t *state, void *user_data){
      dprintf("fin; pressed:%d count:%d \n", state->pressed, state->count);
      tap_state.state = cur_dance(state);
      switch (tap_state.state) {
        case SINGLE_TAP: better_dragscroll_toggle(true); break;
        case SINGLE_HOLD: better_dragscroll_momentary(true); break;
        // case DOUBLE_TAP: break;
        // case DOUBLE_HOLD: break;
        // case DOUBLE_SINGLE_TAP: break;
      }
      // if(state->count >= 2){
      //   better_dragscroll_toggle(true);
      // }
      // else{
      //   better_dragscroll_momentary(true);
      // }
    }
    void dragscrolltd_rst(tap_dance_state_t *state, void *user_data){
      switch (tap_state.state) {
        case SINGLE_TAP: break;
        case SINGLE_HOLD: better_dragscroll_momentary(false); break;
        // case DOUBLE_TAP: break;
        // case DOUBLE_HOLD: break;
        // case DOUBLE_SINGLE_TAP: break;
      }
      tap_state.state = 0;
      // if(state->count < 2){
      //   better_dragscroll_momentary(false);
      // }
    }
  #endif // BETTER_DRAGSCROLL_TAPDANCE

  tap_dance_action_t tap_dance_actions[] = {
    #if defined(BETTER_DRAGSCROLL_TAPDANCE) || defined(BETTER_DRAGSCROLL_TAPDANCE)
      [TD_DRAGSCROLL] = ACTION_TAP_DANCE_FN_ADVANCED(NULL, dragscrolltd_fin, dragscrolltd_rst),
    #endif // BETTER_DRAGSCROLL_TAPDANCE

  };

#endif // TAP_DANCE_ENABLE
